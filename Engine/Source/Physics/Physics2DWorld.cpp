#include "Physics/Physics2DWorld.h"

#include "Physics/Physics2DMath.h"
#include "Physics/PhysicsWorld.h" // LogPhysics

#pragma warning(push, 0)
#include <box2d/box2d.h>
#pragma warning(pop)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>

namespace
{
	constexpr float  CmToM          = 0.01f;
	constexpr float  MToCm          = 100.0f;
	constexpr int    SubStepCount   = 4;
	constexpr uint64 ShapeFlagOneWay = 1u; // 모양 userData 비트 (원웨이 플랫폼)
	constexpr uint64 ShapeFlagMoverProxy = 2u; // 모양 userData 비트 (2D 캐릭터 이동기 대리 모양 — 이동기 질의가 무시)
	constexpr uint64 ShapeFlagSolidProxy = 4u; // 대리 모양 중 다른 캐릭터를 막는 것 (bCollideCharacters 이동기 질의만 본다)

	// 이동기 질의가 이 모양을 거르는가: 대리 모양은 bCollideCharacters 이동기가 막는 대리(SolidProxy)일 때만 본다
	bool IsFilteredProxy(uint64 Flags, bool bCollideCharacters)
	{
		return (Flags & ShapeFlagMoverProxy) != 0 && !(bCollideCharacters && (Flags & ShapeFlagSolidProxy) != 0);
	}

	b2Vec2   ToB2(const FVector2& Centimeters) { return b2Vec2{ Centimeters.X * CmToM, Centimeters.Y * CmToM }; }
	FVector2 FromB2(const b2Vec2& Meters) { return FVector2(Meters.x * MToCm, Meters.y * MToCm); }

	b2BodyType ToB2(EBodyType2D Type)
	{
		switch (Type)
		{
		case EBodyType2D::Static:    return b2_staticBody;
		case EBodyType2D::Kinematic: return b2_kinematicBody;
		default:                     return b2_dynamicBody;
		}
	}

	// 모양 쌍 키 (순서 무관)
	uint64 PairKey(b2ShapeId A, b2ShapeId B)
	{
		const uint64 KeyA = b2StoreShapeId(A);
		const uint64 KeyB = b2StoreShapeId(B);
		return std::min(KeyA, KeyB) * 0x9E3779B97F4A7C15ull ^ std::max(KeyA, KeyB);
	}

	using FBodyPair = std::pair<uint32, uint32>;
	FBodyPair MakePair(uint32 A, uint32 B) { return A < B ? FBodyPair(A, B) : FBodyPair(B, A); }
} // namespace

struct FPhysics2DWorld::FImpl
{
	b2WorldId World = b2_nullWorldId;

	struct FBodySlot
	{
		b2BodyId Body     = b2_nullBodyId;
		uint64   UserData = 0;
		bool     bAlive   = false;
	};
	std::vector<FBodySlot> Bodies;
	std::vector<uint32>    FreeSlots;
	uint32                 AliveCount = 0;
	FCollisionLayerSettings Layers;

	// 바디 쌍 접촉 수 (모양 쌍마다 +1 — 0 ↔ 1에서만 시작/끝을 낸다). 일반 접촉과 트리거는 따로
	std::map<FBodyPair, uint32>         ContactPairs;
	std::map<FBodyPair, uint32>         SensorPairs;
	std::vector<FPhysics2DContactEvent> Pending;

	bool IsValid() const { return b2World_IsValid(World); }

	FBodySlot* Find(uint32 Body)
	{
		return Body < Bodies.size() && Bodies[Body].bAlive && b2Body_IsValid(Bodies[Body].Body) ? &Bodies[Body] : nullptr;
	}
	const FBodySlot* Find(uint32 Body) const
	{
		return Body < Bodies.size() && Bodies[Body].bAlive && b2Body_IsValid(Bodies[Body].Body) ? &Bodies[Body] : nullptr;
	}

	// 모양 → 우리 바디 핸들 (파괴되었거나 우리 바디가 아니면 InvalidBody)
	uint32 BodyOf(b2ShapeId Shape) const
	{
		if (!b2Shape_IsValid(Shape))
		{
			return InvalidBody;
		}
		const b2BodyId Body   = b2Shape_GetBody(Shape);
		const uint64   Handle = reinterpret_cast<uint64>(b2Body_GetUserData(Body));
		return Handle == 0 ? InvalidBody : static_cast<uint32>(Handle - 1);
	}

	b2Filter MakeFilter(uint8 Layer) const
	{
		b2Filter Filter     = b2DefaultFilter();
		const uint32 Slot   = std::min<uint32>(Layer, FCollisionLayerSettings::MaxLayers - 1);
		Filter.categoryBits = uint64(1) << Slot;
		Filter.maskBits     = Layers.GetCollisionMask(Slot);
		return Filter;
	}

	static b2QueryFilter MakeQueryFilter(uint32 LayerMask)
	{
		b2QueryFilter Filter = b2DefaultQueryFilter();
		Filter.categoryBits  = ~uint64(0);
		Filter.maskBits      = LayerMask;
		return Filter;
	}

	void AddPairEvent(std::map<FBodyPair, uint32>& Pairs, bool bSensor, uint32 BodyA, uint32 BodyB, bool bBegin, const FPhysics2DContactEvent* BeginInfo)
	{
		if (BodyA == InvalidBody || BodyB == InvalidBody || BodyA == BodyB)
		{
			return;
		}
		const FBodyPair Pair = MakePair(BodyA, BodyB);
		uint32&         Count = Pairs[Pair];
		if (bBegin)
		{
			if (Count++ != 0)
			{
				return;
			}
		}
		else
		{
			if (Count == 0)
			{
				Pairs.erase(Pair);
				return; // 우리가 끝을 이미 냈다 (바디 파괴 등)
			}
			if (--Count != 0)
			{
				return;
			}
			Pairs.erase(Pair);
		}
		FPhysics2DContactEvent Event;
		if (BeginInfo != nullptr)
		{
			Event = *BeginInfo;
			if (BodyA != Pair.first) // 법선은 Body1 → Body2 방향으로 맞춘다
			{
				Event.Normal = -Event.Normal;
			}
		}
		Event.Type      = bBegin ? EPhysics2DContactEventType::Begin : EPhysics2DContactEventType::End;
		Event.Body1     = Pair.first;
		Event.Body2     = Pair.second;
		Event.UserData1 = Bodies[Pair.first].UserData;
		Event.UserData2 = Bodies[Pair.second].UserData;
		Event.bSensor   = bSensor;
		Pending.push_back(Event);
	}

	// 이 바디가 낀 쌍의 끝을 바로 낸다 (DestroyBody)
	void EndPairsOf(uint32 Body)
	{
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			std::map<FBodyPair, uint32>& Pairs = Pass == 0 ? ContactPairs : SensorPairs;
			for (auto It = Pairs.begin(); It != Pairs.end();)
			{
				if (It->first.first != Body && It->first.second != Body)
				{
					++It;
					continue;
				}
				FPhysics2DContactEvent Event;
				Event.Type      = EPhysics2DContactEventType::End;
				Event.Body1     = It->first.first;
				Event.Body2     = It->first.second;
				Event.UserData1 = Bodies[Event.Body1].UserData;
				Event.UserData2 = Bodies[Event.Body2].UserData;
				Event.bSensor   = Pass == 1;
				Pending.push_back(Event);
				It = Pairs.erase(It);
			}
		}
	}

	// 원웨이 플랫폼 판정 (Box2D 사전 해결 콜백 — 이 월드는 작업 스레드가 없어 Step을 부른 스레드에서 불린다. 읽기만 한다).
	// 플랫폼(원웨이 모양)에서 상대 쪽 법선이 플랫폼 위쪽(바디 로컬 +Y)과 60도 안이고, 상대가 플랫폼에 대해 위로 빠르게 올라가는 중이 아니면 막는다
	static bool PreSolve(b2ShapeId ShapeA, b2ShapeId ShapeB, b2Manifold* Manifold, void* /*Context*/)
	{
		const bool bOneWayA = (reinterpret_cast<uint64>(b2Shape_GetUserData(ShapeA)) & ShapeFlagOneWay) != 0;
		const bool bOneWayB = (reinterpret_cast<uint64>(b2Shape_GetUserData(ShapeB)) & ShapeFlagOneWay) != 0;
		if (bOneWayA == bOneWayB)
		{
			return true; // 둘 다 원웨이거나 둘 다 아님 — 보통 접촉
		}
		const b2ShapeId Platform = bOneWayA ? ShapeA : ShapeB;
		const b2ShapeId Other    = bOneWayA ? ShapeB : ShapeA;
		// 매니폴드 법선은 A → B. 플랫폼 → 상대 방향으로
		const b2Vec2   Normal   = bOneWayA ? Manifold->normal : b2Vec2{ -Manifold->normal.x, -Manifold->normal.y };
		const b2BodyId PlatformBody = b2Shape_GetBody(Platform);
		const b2BodyId OtherBody    = b2Shape_GetBody(Other);
		const b2Rot    Rotation     = b2Body_GetRotation(PlatformBody);
		const b2Vec2   Up           = b2RotateVector(Rotation, b2Vec2{ 0.0f, 1.0f });
		if (Normal.x * Up.x + Normal.y * Up.y < 0.5f)
		{
			return false; // 아래·옆에서 닿음 — 통과
		}
		const b2Vec2 Relative = b2Sub(b2Body_GetLinearVelocity(OtherBody), b2Body_GetLinearVelocity(PlatformBody));
		const float  Rising   = Relative.x * Up.x + Relative.y * Up.y;
		return Rising <= 0.5f; // m/s — 아래에서 뚫고 올라오는 중이면 끝까지 통과시킨다
	}

	void CollectEvents()
	{
		// 같은 스텝의 충돌(hit) 이벤트 → 시작 이벤트의 다가오던 속력
		const b2ContactEvents Contacts = b2World_GetContactEvents(World);
		std::unordered_map<uint64, float> HitSpeeds;
		for (int Index = 0; Index < Contacts.hitCount; ++Index)
		{
			const b2ContactHitEvent& Hit = Contacts.hitEvents[Index];
			HitSpeeds[PairKey(Hit.shapeIdA, Hit.shapeIdB)] = Hit.approachSpeed;
		}
		for (int Index = 0; Index < Contacts.beginCount; ++Index)
		{
			const b2ContactBeginTouchEvent& Begin = Contacts.beginEvents[Index];
			const uint32 BodyA = BodyOf(Begin.shapeIdA);
			const uint32 BodyB = BodyOf(Begin.shapeIdB);
			if (BodyA == InvalidBody || BodyB == InvalidBody)
			{
				continue;
			}
			FPhysics2DContactEvent Info;
			const b2Manifold& Manifold = Begin.manifold;
			b2Vec2 Point = b2Vec2_zero;
			for (int PointIndex = 0; PointIndex < Manifold.pointCount; ++PointIndex)
			{
				Point = b2Add(Point, Manifold.points[PointIndex].point);
			}
			if (Manifold.pointCount > 0)
			{
				Point = b2MulSV(1.0f / static_cast<float>(Manifold.pointCount), Point);
			}
			Info.Position = FromB2(Point);
			Info.Normal   = FVector2(Manifold.normal.x, Manifold.normal.y); // A → B = B를 A에서 밀어내는 방향
			const auto Found = HitSpeeds.find(PairKey(Begin.shapeIdA, Begin.shapeIdB));
			Info.ApproachSpeed = Found != HitSpeeds.end() ? Found->second * MToCm : 0.0f;
			const float MassA  = b2Body_GetType(Bodies[BodyA].Body) == b2_dynamicBody ? b2Body_GetMass(Bodies[BodyA].Body) : 0.0f;
			const float MassB  = b2Body_GetType(Bodies[BodyB].Body) == b2_dynamicBody ? b2Body_GetMass(Bodies[BodyB].Body) : 0.0f;
			const float Reduced = MassA > 0.0f && MassB > 0.0f ? MassA * MassB / (MassA + MassB) : std::max(MassA, MassB);
			Info.Impulse = Info.ApproachSpeed * Reduced;
			AddPairEvent(ContactPairs, false, BodyA, BodyB, true, &Info);
		}
		for (int Index = 0; Index < Contacts.endCount; ++Index)
		{
			const b2ContactEndTouchEvent& End = Contacts.endEvents[Index];
			AddPairEvent(ContactPairs, false, BodyOf(End.shapeIdA), BodyOf(End.shapeIdB), false, nullptr);
		}

		const b2SensorEvents Sensors = b2World_GetSensorEvents(World);
		const auto AcceptVisitor = [](b2ShapeId Visitor) {
			return b2Shape_IsValid(Visitor) && !b2Shape_IsSensor(Visitor) && b2Body_GetType(b2Shape_GetBody(Visitor)) != b2_staticBody;
		};
		for (int Index = 0; Index < Sensors.beginCount; ++Index)
		{
			const b2SensorBeginTouchEvent& Begin = Sensors.beginEvents[Index];
			if (AcceptVisitor(Begin.visitorShapeId))
			{
				AddPairEvent(SensorPairs, true, BodyOf(Begin.sensorShapeId), BodyOf(Begin.visitorShapeId), true, nullptr);
			}
		}
		for (int Index = 0; Index < Sensors.endCount; ++Index)
		{
			const b2SensorEndTouchEvent& End = Sensors.endEvents[Index];
			// 파괴된 모양이면 BodyOf가 무효 — 그 쌍은 DestroyBody가 이미 끝을 냈다
			if (b2Shape_IsValid(End.visitorShapeId) && !AcceptVisitor(End.visitorShapeId))
			{
				continue;
			}
			AddPairEvent(SensorPairs, true, BodyOf(End.sensorShapeId), BodyOf(End.visitorShapeId), false, nullptr);
		}
	}
};

FPhysics2DWorld::FPhysics2DWorld() : Impl(std::make_unique<FImpl>())
{
	b2WorldDef Def = b2DefaultWorldDef();
	Def.gravity    = b2Vec2{ 0.0f, -9.80665f };
	// 같은 스텝 충돌 속력(시작 이벤트의 다가오던 속력)을 거의 모든 충돌에서 얻는다
	Def.hitEventThreshold = 0.01f;
	Impl->World           = b2CreateWorld(&Def);
	if (!Impl->IsValid())
	{
		E_LOG(LogPhysics, Error, "Box2D 월드를 만들지 못했습니다 (동시 월드 수 한도)");
		return;
	}
	b2World_SetPreSolveCallback(Impl->World, &FImpl::PreSolve, nullptr);
}

FPhysics2DWorld::~FPhysics2DWorld()
{
	if (Impl->IsValid())
	{
		b2DestroyWorld(Impl->World);
	}
}

bool FPhysics2DWorld::IsValid() const
{
	return Impl->IsValid();
}

uint32 FPhysics2DWorld::CreateBody(const FPhysics2DBodyDesc& Desc)
{
	if (!Impl->IsValid() || Desc.Shapes.empty())
	{
		return InvalidBody;
	}
	uint32 Handle = InvalidBody;
	if (!Impl->FreeSlots.empty())
	{
		Handle = Impl->FreeSlots.back();
		Impl->FreeSlots.pop_back();
	}
	else
	{
		Handle = static_cast<uint32>(Impl->Bodies.size());
		Impl->Bodies.emplace_back();
	}

	b2BodyDef BodyDef      = b2DefaultBodyDef();
	BodyDef.type           = ToB2(Desc.Type);
	BodyDef.position       = ToB2(Desc.Position);
	BodyDef.rotation       = b2MakeRot(Desc.Angle);
	BodyDef.gravityScale   = Desc.GravityScale;
	BodyDef.linearDamping  = std::max(Desc.LinearDamping, 0.0f);
	BodyDef.angularDamping = std::max(Desc.AngularDamping, 0.0f);
	BodyDef.fixedRotation  = Desc.bFixedRotation;
	BodyDef.isBullet       = Desc.bBullet;
	BodyDef.userData       = reinterpret_cast<void*>(static_cast<uintptr_t>(Handle) + 1u);
	const b2BodyId Body    = b2CreateBody(Impl->World, &BodyDef);

	uint32 ShapeCount = 0;
	for (const FPhysics2DShapeDesc& Shape : Desc.Shapes)
	{
		const b2Filter Filter   = Impl->MakeFilter(Shape.CollisionLayer);
		void* const    UserData = reinterpret_cast<void*>(
			static_cast<uintptr_t>((Shape.bOneWay ? ShapeFlagOneWay : 0u) | (Shape.bMoverProxy ? ShapeFlagMoverProxy : 0u) |
			                       (Shape.bMoverProxy && Shape.bSolidProxy ? ShapeFlagSolidProxy : 0u)));

		b2ShapeDef ShapeDef           = b2DefaultShapeDef();
		ShapeDef.userData             = UserData;
		ShapeDef.material.friction    = std::max(Shape.Friction, 0.0f);
		ShapeDef.material.restitution = std::max(Shape.Restitution, 0.0f);
		ShapeDef.density              = std::max(Shape.Density, 0.0f);
		ShapeDef.filter               = Filter;
		ShapeDef.isSensor             = Shape.bIsTrigger;
		ShapeDef.enableSensorEvents   = true; // 트리거는 스스로 감지, 아닌 모양은 트리거에 감지된다
		ShapeDef.enableContactEvents  = !Shape.bIsTrigger && Desc.bReportContacts;
		ShapeDef.enableHitEvents      = !Shape.bIsTrigger && Desc.bReportContacts;
		ShapeDef.enablePreSolveEvents = !Shape.bIsTrigger; // 원웨이 플랫폼 판정 (원웨이 모양이 없는 쌍은 바로 통과)
		ShapeDef.updateBodyMass       = false;             // 모양을 다 만든 뒤 한 번

		const b2Vec2 Offset = ToB2(Shape.Offset);
		switch (Shape.Shape)
		{
		case EPhysics2DShape::Box:
		{
			const float HalfX = std::max(std::abs(Shape.HalfSize.X) * CmToM, 0.001f);
			const float HalfY = std::max(std::abs(Shape.HalfSize.Y) * CmToM, 0.001f);
			const b2Polygon Box = b2MakeOffsetBox(HalfX, HalfY, Offset, b2MakeRot(Shape.Angle));
			b2CreatePolygonShape(Body, &ShapeDef, &Box);
			++ShapeCount;
			break;
		}
		case EPhysics2DShape::Circle:
		{
			const b2Circle Circle{ Offset, std::max(std::abs(Shape.Radius) * CmToM, 0.001f) };
			b2CreateCircleShape(Body, &ShapeDef, &Circle);
			++ShapeCount;
			break;
		}
		case EPhysics2DShape::Capsule:
		{
			const float Radius = std::max(std::abs(Shape.Radius) * CmToM, 0.001f);
			const float Half   = std::abs(Shape.HalfSegment) * CmToM;
			if (Half <= 0.0025f) // 반원 중심이 거의 겹침 — 원
			{
				const b2Circle Circle{ Offset, Radius };
				b2CreateCircleShape(Body, &ShapeDef, &Circle);
			}
			else
			{
				const b2Capsule Capsule{ b2Vec2{ Offset.x, Offset.y - Half }, b2Vec2{ Offset.x, Offset.y + Half }, Radius };
				b2CreateCapsuleShape(Body, &ShapeDef, &Capsule);
			}
			++ShapeCount;
			break;
		}
		case EPhysics2DShape::Polygon:
		{
			if (Shape.Points.size() < 3 || Shape.Points.size() > B2_MAX_POLYGON_VERTICES)
			{
				break;
			}
			b2Vec2 Points[B2_MAX_POLYGON_VERTICES];
			for (size_t Index = 0; Index < Shape.Points.size(); ++Index)
			{
				Points[Index] = b2Add(ToB2(Shape.Points[Index]), Offset);
			}
			const b2Hull Hull = b2ComputeHull(Points, static_cast<int>(Shape.Points.size()));
			if (Hull.count < 3)
			{
				E_LOG(LogPhysics, Warning, "2D 다각형 콜라이더가 너무 작거나 납작해 만들지 않았습니다 (엔티티 {})", Desc.UserData);
				break;
			}
			const b2Polygon Polygon = b2MakePolygon(&Hull, 0.0f);
			b2CreatePolygonShape(Body, &ShapeDef, &Polygon);
			++ShapeCount;
			break;
		}
		case EPhysics2DShape::Edge:
		{
			std::vector<b2Vec2> Points;
			Points.reserve(Shape.Points.size());
			for (const FVector2& Point : Shape.Points)
			{
				Points.push_back(b2Add(ToB2(Point), Offset));
			}
			if (Points.size() < 2)
			{
				break;
			}
			if (Shape.bLoop && Points.size() >= 4 && !Shape.bIsTrigger)
			{
				// 닫힌 체인: 반시계로 감아 바깥쪽이 막히게
				std::vector<FVector2> Plane(Shape.Points.begin(), Shape.Points.end());
				if (Physics2DMath::SignedArea(Plane) < 0.0f)
				{
					std::reverse(Points.begin(), Points.end());
				}
				b2SurfaceMaterial Material = b2DefaultSurfaceMaterial();
				Material.friction          = ShapeDef.material.friction;
				Material.restitution       = ShapeDef.material.restitution;
				b2ChainDef ChainDef        = b2DefaultChainDef();
				ChainDef.userData          = UserData;
				ChainDef.points            = Points.data();
				ChainDef.count             = static_cast<int>(Points.size());
				ChainDef.materials         = &Material;
				ChainDef.materialCount     = 1;
				ChainDef.filter            = Filter;
				ChainDef.isLoop            = true;
				ChainDef.enableSensorEvents = true;
				b2CreateChain(Body, &ChainDef);
				++ShapeCount;
				break;
			}
			const size_t SegmentCount = Shape.bLoop && Points.size() >= 3 ? Points.size() : Points.size() - 1;
			for (size_t Index = 0; Index < SegmentCount; ++Index)
			{
				const b2Segment Segment{ Points[Index], Points[(Index + 1) % Points.size()] };
				if (b2DistanceSquared(Segment.point1, Segment.point2) < 1.0e-8f)
				{
					continue;
				}
				b2CreateSegmentShape(Body, &ShapeDef, &Segment);
				++ShapeCount;
			}
			break;
		}
		}
	}
	if (ShapeCount == 0)
	{
		b2DestroyBody(Body);
		Impl->FreeSlots.push_back(Handle);
		return InvalidBody;
	}
	if (Desc.Type == EBodyType2D::Dynamic)
	{
		b2Body_ApplyMassFromShapes(Body);
		if (Desc.Mass > 0.0f)
		{
			b2MassData Mass = b2Body_GetMassData(Body);
			if (Mass.mass > 0.0f)
			{
				const float Scale      = Desc.Mass / Mass.mass;
				Mass.mass              = Desc.Mass;
				Mass.rotationalInertia = Mass.rotationalInertia * Scale;
			}
			else
			{
				Mass.mass = Desc.Mass; // 넓이 없는 모양(선분)뿐 — 회전 관성은 없는 채로
			}
			b2Body_SetMassData(Body, Mass);
		}
	}

	FImpl::FBodySlot& Slot = Impl->Bodies[Handle];
	Slot.Body              = Body;
	Slot.UserData          = Desc.UserData;
	Slot.bAlive            = true;
	++Impl->AliveCount;
	return Handle;
}

void FPhysics2DWorld::DestroyBody(uint32 Body)
{
	FImpl::FBodySlot* Slot = Impl->Find(Body);
	if (Slot == nullptr)
	{
		return;
	}
	Impl->EndPairsOf(Body);
	b2DestroyBody(Slot->Body);
	Slot->Body   = b2_nullBodyId;
	Slot->bAlive = false;
	Impl->FreeSlots.push_back(Body);
	--Impl->AliveCount;
}

uint32 FPhysics2DWorld::GetBodyCount() const
{
	return Impl->AliveCount;
}

void FPhysics2DWorld::SetTransform(uint32 Body, const FVector2& Position, float Angle)
{
	if (FImpl::FBodySlot* Slot = Impl->Find(Body))
	{
		b2Body_SetTransform(Slot->Body, ToB2(Position), b2MakeRot(Angle));
		// 정적 바디를 옮기면 Box2D는 닿아 있던 잠든 바디를 깨우지 않는다 → 받침이 사라져도 공중에 떠 있으므로 직접 깨운다
		if (b2Body_GetType(Slot->Body) == b2_staticBody)
		{
			const int32 Capacity = b2Body_GetContactCapacity(Slot->Body);
			if (Capacity > 0)
			{
				std::vector<b2ContactData> Contacts(static_cast<size_t>(Capacity));
				const int32 Count = b2Body_GetContactData(Slot->Body, Contacts.data(), Capacity);
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const b2BodyId BodyA = b2Shape_GetBody(Contacts[static_cast<size_t>(Index)].shapeIdA);
					const b2BodyId BodyB = b2Shape_GetBody(Contacts[static_cast<size_t>(Index)].shapeIdB);
					const b2BodyId Other = B2_ID_EQUALS(BodyA, Slot->Body) ? BodyB : BodyA;
					if (b2Body_GetType(Other) != b2_staticBody)
					{
						b2Body_SetAwake(Other, true);
					}
				}
			}
		}
	}
}

void FPhysics2DWorld::MoveKinematic(uint32 Body, const FVector2& Position, float Angle, float DeltaSeconds)
{
	if (FImpl::FBodySlot* Slot = Impl->Find(Body); Slot != nullptr && DeltaSeconds > 0.0f)
	{
		b2Body_SetTargetTransform(Slot->Body, b2Transform{ ToB2(Position), b2MakeRot(Angle) }, DeltaSeconds);
	}
}

bool FPhysics2DWorld::GetTransform(uint32 Body, FVector2& OutPosition, float& OutAngle) const
{
	const FImpl::FBodySlot* Slot = Impl->Find(Body);
	if (Slot == nullptr)
	{
		return false;
	}
	const b2Transform Transform = b2Body_GetTransform(Slot->Body);
	OutPosition                 = FromB2(Transform.p);
	OutAngle                    = b2Rot_GetAngle(Transform.q);
	return true;
}

void FPhysics2DWorld::AddForce(uint32 Body, const FVector2& Force)
{
	if (FImpl::FBodySlot* Slot = Impl->Find(Body))
	{
		b2Body_ApplyForceToCenter(Slot->Body, ToB2(Force), true); // kg·cm/s² → N
	}
}

void FPhysics2DWorld::AddImpulse(uint32 Body, const FVector2& Impulse)
{
	if (FImpl::FBodySlot* Slot = Impl->Find(Body))
	{
		b2Body_ApplyLinearImpulseToCenter(Slot->Body, ToB2(Impulse), true);
	}
}

void FPhysics2DWorld::SetLinearVelocity(uint32 Body, const FVector2& Velocity)
{
	if (FImpl::FBodySlot* Slot = Impl->Find(Body))
	{
		b2Body_SetLinearVelocity(Slot->Body, ToB2(Velocity));
	}
}

FVector2 FPhysics2DWorld::GetLinearVelocity(uint32 Body) const
{
	const FImpl::FBodySlot* Slot = Impl->Find(Body);
	return Slot != nullptr ? FromB2(b2Body_GetLinearVelocity(Slot->Body)) : FVector2();
}

void FPhysics2DWorld::SetAngularVelocity(uint32 Body, float RadiansPerSecond)
{
	if (FImpl::FBodySlot* Slot = Impl->Find(Body))
	{
		b2Body_SetAngularVelocity(Slot->Body, RadiansPerSecond);
	}
}

float FPhysics2DWorld::GetAngularVelocity(uint32 Body) const
{
	const FImpl::FBodySlot* Slot = Impl->Find(Body);
	return Slot != nullptr ? b2Body_GetAngularVelocity(Slot->Body) : 0.0f;
}

float FPhysics2DWorld::GetMass(uint32 Body) const
{
	const FImpl::FBodySlot* Slot = Impl->Find(Body);
	return Slot != nullptr && b2Body_GetType(Slot->Body) == b2_dynamicBody ? b2Body_GetMass(Slot->Body) : 0.0f;
}

void FPhysics2DWorld::SetGravity(const FVector2& Gravity)
{
	if (Impl->IsValid())
	{
		b2World_SetGravity(Impl->World, ToB2(Gravity));
	}
}

FVector2 FPhysics2DWorld::GetGravity() const
{
	return Impl->IsValid() ? FromB2(b2World_GetGravity(Impl->World)) : FVector2();
}

void FPhysics2DWorld::SetCollisionLayers(const FCollisionLayerSettings& Layers)
{
	Impl->Layers = Layers;
}

void FPhysics2DWorld::Step(float DeltaSeconds)
{
	if (!Impl->IsValid() || DeltaSeconds <= 0.0f)
	{
		return;
	}
	b2World_Step(Impl->World, DeltaSeconds, SubStepCount);
	Impl->CollectEvents();
}

bool FPhysics2DWorld::Raycast(const FVector2& Origin, const FVector2& Direction, float MaxDistance, FPhysics2DRayHit& OutHit, uint32 LayerMask) const
{
	const float Length = Direction.Length();
	if (!Impl->IsValid() || Length <= 1.0e-6f || MaxDistance <= 0.0f)
	{
		return false;
	}
	const FVector2 Unit        = Direction / Length;
	const b2Vec2   Translation = ToB2(Unit * MaxDistance);
	struct FContext
	{
		b2ShapeId Shape = b2_nullShapeId;
		b2Vec2    Point{};
		b2Vec2    Normal{};
		float     Fraction = 1.0f;
		bool      bHit     = false;
	} Context;
	b2World_CastRay(
		Impl->World, ToB2(Origin), Translation, FImpl::MakeQueryFilter(LayerMask),
		[](b2ShapeId Shape, b2Vec2 Point, b2Vec2 Normal, float Fraction, void* UserContext) -> float {
			if (b2Shape_IsSensor(Shape))
			{
				return -1.0f; // 트리거는 건너뛴다
			}
			FContext& Found = *static_cast<FContext*>(UserContext);
			Found.Shape     = Shape;
			Found.Point     = Point;
			Found.Normal    = Normal;
			Found.Fraction  = Fraction;
			Found.bHit      = true;
			return Fraction; // 더 가까운 것만 계속 찾는다
		},
		&Context);
	if (!Context.bHit)
	{
		return false;
	}
	const uint32 Body = Impl->BodyOf(Context.Shape);
	OutHit.UserData   = Body != InvalidBody ? Impl->Bodies[Body].UserData : 0;
	OutHit.Position   = FromB2(Context.Point);
	OutHit.Normal     = FVector2(Context.Normal.x, Context.Normal.y);
	OutHit.Fraction   = Context.Fraction;
	OutHit.Distance   = Context.Fraction * MaxDistance;
	return true;
}

uint32 FPhysics2DWorld::OverlapBox(const FVector2& Center, const FVector2& HalfSize, float Angle, std::vector<uint64>& OutUserData, uint32 LayerMask) const
{
	if (!Impl->IsValid())
	{
		return 0;
	}
	const b2Polygon Box = b2MakeOffsetBox(std::max(std::abs(HalfSize.X) * CmToM, 0.0005f), std::max(std::abs(HalfSize.Y) * CmToM, 0.0005f),
	                                      ToB2(Center), b2MakeRot(Angle));
	const b2ShapeProxy Proxy = b2MakeProxy(Box.vertices, Box.count, 0.0f);
	std::vector<uint32> Found;
	struct FContext
	{
		const FImpl*         Owner;
		std::vector<uint32>* Found;
	} Context{ Impl.get(), &Found };
	b2World_OverlapShape(
		Impl->World, &Proxy, FImpl::MakeQueryFilter(LayerMask),
		[](b2ShapeId Shape, void* UserContext) -> bool {
			const FContext& Self = *static_cast<FContext*>(UserContext);
			if (!b2Shape_IsSensor(Shape))
			{
				const uint32 Body = Self.Owner->BodyOf(Shape);
				if (Body != InvalidBody && std::find(Self.Found->begin(), Self.Found->end(), Body) == Self.Found->end())
				{
					Self.Found->push_back(Body);
				}
			}
			return true;
		},
		&Context);
	for (const uint32 Body : Found)
	{
		OutUserData.push_back(Impl->Bodies[Body].UserData);
	}
	return static_cast<uint32>(Found.size());
}

uint32 FPhysics2DWorld::OverlapCircle(const FVector2& Center, float Radius, std::vector<uint64>& OutUserData, uint32 LayerMask) const
{
	if (!Impl->IsValid())
	{
		return 0;
	}
	const b2Vec2       Point = ToB2(Center);
	const b2ShapeProxy Proxy = b2MakeProxy(&Point, 1, std::max(std::abs(Radius) * CmToM, 0.0005f));
	std::vector<uint32> Found;
	struct FContext
	{
		const FImpl*         Owner;
		std::vector<uint32>* Found;
	} Context{ Impl.get(), &Found };
	b2World_OverlapShape(
		Impl->World, &Proxy, FImpl::MakeQueryFilter(LayerMask),
		[](b2ShapeId Shape, void* UserContext) -> bool {
			const FContext& Self = *static_cast<FContext*>(UserContext);
			if (!b2Shape_IsSensor(Shape))
			{
				const uint32 Body = Self.Owner->BodyOf(Shape);
				if (Body != InvalidBody && std::find(Self.Found->begin(), Self.Found->end(), Body) == Self.Found->end())
				{
					Self.Found->push_back(Body);
				}
			}
			return true;
		},
		&Context);
	for (const uint32 Body : Found)
	{
		OutUserData.push_back(Impl->Bodies[Body].UserData);
	}
	return static_cast<uint32>(Found.size());
}

void FPhysics2DWorld::SetBodyReportsContacts(uint32 Body, bool bReport)
{
	FImpl::FBodySlot* Slot = Impl->Find(Body);
	if (Slot == nullptr)
	{
		return;
	}
	std::vector<b2ShapeId> Shapes(static_cast<size_t>(std::max(b2Body_GetShapeCount(Slot->Body), 0)));
	const int Count = b2Body_GetShapes(Slot->Body, Shapes.data(), static_cast<int>(Shapes.size()));
	for (int Index = 0; Index < Count; ++Index)
	{
		if (!b2Shape_IsSensor(Shapes[Index]))
		{
			b2Shape_EnableContactEvents(Shapes[Index], bReport);
			b2Shape_EnableHitEvents(Shapes[Index], bReport);
		}
	}
}

void FPhysics2DWorld::ConsumeContactEvents(std::vector<FPhysics2DContactEvent>& OutEvents)
{
	OutEvents.insert(OutEvents.end(), Impl->Pending.begin(), Impl->Pending.end());
	Impl->Pending.clear();
}

// ---------------------------------------------------------------- 2D 캐릭터 이동기 (FPhysics2DMover 주석, Physics/CharacterMovement2D.h)
// Box2D 캐릭터 이동 도구(b2World_CollideMover/b2SolvePlanes/b2ClipVector)를 쓰고, 캐스트는 b2World_CastMover와 같은 식(b2ShapeCast +
// 겹침 진입 허용, 처음부터 겹친 모양 무시)을 모양마다 직접 돌린다 — b2World_CastMover에는 모양 거르기 콜백이 없어 원웨이·대리 모양을
// 걸러 낼 수 없기 때문. 콜백은 모으기만 하고 거르기·변환은 여기(메인 스레드, 월드 잠김 없음)서 한다.

namespace
{
	constexpr int   MoverIterations    = 5;
	constexpr float MoverToleranceCm   = 1.0f;  // 이번 반복 이동이 이보다 작으면 끝
	constexpr float OneWayUpCos        = 0.5f;  // 60도
	constexpr float OneWayRisingCmPerS = 50.0f; // 바디 사전 해결 콜백과 같은 0.5m/s
	constexpr float MinMoverRadiusCm   = 1.5f;  // Box2D: 반지름 > 2 × 선형 여유(0.5cm)
	constexpr float InternalEdgeToleranceCm = 1.0f; // 내부 모서리 판정: 바닥 면에서 이 거리 안

	b2Capsule MakeMoverCapsule(const FPhysics2DMover& Mover, const FVector2& Position, float Inflate)
	{
		const b2Vec2 Center = ToB2(Position);
		const float  Half   = std::max(Mover.HalfSegment, 0.0f) * CmToM;
		b2Capsule    Capsule;
		Capsule.center1 = b2Vec2{ Center.x, Center.y - Half };
		Capsule.center2 = b2Vec2{ Center.x, Center.y + Half };
		Capsule.radius  = std::max(Mover.Radius + Inflate, MinMoverRadiusCm) * CmToM;
		return Capsule;
	}

	EBodyType2D ToBodyType(b2BodyType Type)
	{
		return Type == b2_staticBody ? EBodyType2D::Static : (Type == b2_kinematicBody ? EBodyType2D::Kinematic : EBodyType2D::Dynamic);
	}

	FVector2 GetBodyUp(b2BodyId Body)
	{
		const b2Vec2 Up = b2RotateVector(b2Body_GetRotation(Body), b2Vec2{ 0.0f, 1.0f });
		return FVector2(Up.x, Up.y);
	}

	// 월드 좌표 모양 대리 (캐스트용). 모르는 모양이면 false
	bool MakeWorldShapeProxy(b2ShapeId Shape, b2ShapeProxy& OutProxy)
	{
		const b2Transform Transform = b2Body_GetTransform(b2Shape_GetBody(Shape));
		switch (b2Shape_GetType(Shape))
		{
		case b2_polygonShape:
		{
			const b2Polygon Polygon = b2Shape_GetPolygon(Shape);
			OutProxy                = b2MakeOffsetProxy(Polygon.vertices, Polygon.count, Polygon.radius, Transform.p, Transform.q);
			return true;
		}
		case b2_circleShape:
		{
			const b2Circle Circle = b2Shape_GetCircle(Shape);
			OutProxy              = b2MakeOffsetProxy(&Circle.center, 1, Circle.radius, Transform.p, Transform.q);
			return true;
		}
		case b2_capsuleShape:
		{
			const b2Capsule Capsule   = b2Shape_GetCapsule(Shape);
			const b2Vec2    Points[2] = { Capsule.center1, Capsule.center2 };
			OutProxy                  = b2MakeOffsetProxy(Points, 2, Capsule.radius, Transform.p, Transform.q);
			return true;
		}
		case b2_segmentShape:
		{
			const b2Segment Segment   = b2Shape_GetSegment(Shape);
			const b2Vec2    Points[2] = { Segment.point1, Segment.point2 };
			OutProxy                  = b2MakeOffsetProxy(Points, 2, 0.0f, Transform.p, Transform.q);
			return true;
		}
		case b2_chainSegmentShape:
		{
			const b2ChainSegment Chain     = b2Shape_GetChainSegment(Shape);
			const b2Vec2         Points[2] = { Chain.segment.point1, Chain.segment.point2 };
			OutProxy                       = b2MakeOffsetProxy(Points, 2, 0.0f, Transform.p, Transform.q);
			return true;
		}
		default: return false;
		}
	}
} // namespace

void FPhysics2DWorld::CollideMover(const FPhysics2DMover& Mover, const FVector2& Position, float Inflate,
                                   std::vector<FPhysics2DMoverContact>& OutContacts) const
{
	OutContacts.clear();
	if (!Impl->IsValid())
	{
		return;
	}
	struct FRaw
	{
		b2ShapeId     Shape;
		b2PlaneResult Result;
	};
	std::vector<FRaw> Raw;
	const b2Capsule   Capsule = MakeMoverCapsule(Mover, Position, Inflate);
	const b2Filter    Filter  = Impl->MakeFilter(Mover.CollisionLayer);
	b2World_CollideMover(
		Impl->World, &Capsule, b2QueryFilter{ Filter.categoryBits, Filter.maskBits },
		[](b2ShapeId Shape, const b2PlaneResult* Plane, void* Context) -> bool {
			static_cast<std::vector<FRaw>*>(Context)->push_back({ Shape, *Plane });
			return true;
		},
		&Raw);
	const float InflateCm = std::max(Mover.Radius + Inflate, MinMoverRadiusCm) - std::max(Mover.Radius, MinMoverRadiusCm);
	for (const FRaw& Item : Raw)
	{
		const uint64 Flags = reinterpret_cast<uint64>(b2Shape_GetUserData(Item.Shape));
		const uint32 Body  = Impl->BodyOf(Item.Shape);
		if (b2Shape_IsSensor(Item.Shape) || IsFilteredProxy(Flags, Mover.bCollideCharacters) || Body == InvalidBody ||
		    (Mover.IgnoreUserData != 0 && Impl->Bodies[Body].UserData == Mover.IgnoreUserData))
		{
			continue;
		}
		const b2BodyId         BodyId = Impl->Bodies[Body].Body;
		// b2CollideMover는 법선만 월드로 돌려 주고 점은 모양(바디) 로컬로 남긴다 (Box2D v3.1.1) — 여기서 월드로
		const b2Vec2           WorldPoint = b2TransformPoint(b2Body_GetTransform(BodyId), Item.Result.point);
		FPhysics2DMoverContact Contact;
		Contact.Normal      = FVector2(Item.Result.plane.normal.x, Item.Result.plane.normal.y);
		Contact.Point       = FromB2(WorldPoint);
		Contact.Penetration = Item.Result.plane.offset * MToCm - InflateCm;
		Contact.Body        = Body;
		Contact.UserData    = Impl->Bodies[Body].UserData;
		Contact.bOneWay     = (Flags & ShapeFlagOneWay) != 0;
		Contact.bCharacter  = (Flags & ShapeFlagMoverProxy) != 0;
		Contact.BodyType    = ToBodyType(b2Body_GetType(BodyId));
		if (Contact.bOneWay)
		{
			if (Mover.bIgnoreOneWay)
			{
				continue;
			}
			const FVector2 Up       = GetBodyUp(BodyId);
			const FVector2 Relative = Mover.Velocity - FromB2(b2Body_GetWorldPointVelocity(BodyId, WorldPoint));
			if (FVector2::Dot(Contact.Normal, Up) < OneWayUpCos || Contact.Penetration > Mover.OneWayMaxPenetration ||
			    FVector2::Dot(Relative, Up) > OneWayRisingCmPerS)
			{
				continue; // 아래·옆에서 / 깊이 묻힘 / 뚫고 올라가는 중
			}
		}
		if (Contact.BodyType == EBodyType2D::Dynamic && Contact.Normal.Y < Mover.WalkableNormalY)
		{
			continue; // 동적 바디는 위에 설 때만 막는다 (옆은 대리 바디가 민다)
		}
		OutContacts.push_back(Contact);
	}
	// 내부 모서리 거르기 (타일 이음매·경사 끝의 고스트 접촉): 바닥이 아닌 접촉의 점이 다른 바닥 접촉의 면 위(또는 아래)에 있으면
	// 드러난 면이 아니라 이어진 바닥 속 꼭짓점이다 — 버린다 (안 버리면 경사를 오르다 꼭짓점 법선에 막힌다). 바닥 접촉은 항상 남긴다
	std::vector<bool> Internal(OutContacts.size(), false);
	for (size_t Index = 0; Index < OutContacts.size(); ++Index)
	{
		const FPhysics2DMoverContact& Contact = OutContacts[Index];
		if (Contact.Normal.Y >= Mover.WalkableNormalY)
		{
			continue;
		}
		for (const FPhysics2DMoverContact& Floor : OutContacts)
		{
			if (&Floor != &Contact && Floor.Normal.Y >= Mover.WalkableNormalY && FVector2::Dot(Contact.Point - Floor.Point, Floor.Normal) <= InternalEdgeToleranceCm)
			{
				Internal[Index] = true;
				break;
			}
		}
	}
	size_t Kept = 0;
	for (size_t Index = 0; Index < OutContacts.size(); ++Index)
	{
		if (!Internal[Index])
		{
			OutContacts[Kept++] = OutContacts[Index];
		}
	}
	OutContacts.resize(Kept);
}

bool FPhysics2DWorld::CastMover(const FPhysics2DMover& Mover, const FVector2& Position, const FVector2& Translation, float& OutFraction,
                                FPhysics2DMoverContact& OutHit) const
{
	OutFraction = 1.0f;
	if (!Impl->IsValid() || Translation.LengthSquared() < 1.0e-8f)
	{
		return false;
	}
	const b2Capsule Capsule = MakeMoverCapsule(Mover, Position, 0.0f);
	const b2Vec2    Move    = ToB2(Translation);
	const b2Vec2    Extent{ Capsule.radius, Capsule.radius };
	b2AABB          Bounds;
	Bounds.lowerBound = b2Sub(b2Min(Capsule.center1, Capsule.center2), Extent);
	Bounds.upperBound = b2Add(b2Max(Capsule.center1, Capsule.center2), Extent);
	Bounds.lowerBound = b2Min(Bounds.lowerBound, b2Add(Bounds.lowerBound, Move));
	Bounds.upperBound = b2Max(Bounds.upperBound, b2Add(Bounds.upperBound, Move));

	std::vector<b2ShapeId> Candidates;
	const b2Filter         Filter = Impl->MakeFilter(Mover.CollisionLayer);
	b2World_OverlapAABB(
		Impl->World, Bounds, b2QueryFilter{ Filter.categoryBits, Filter.maskBits },
		[](b2ShapeId Shape, void* Context) -> bool {
			static_cast<std::vector<b2ShapeId>*>(Context)->push_back(Shape);
			return true;
		},
		&Candidates);

	const b2Vec2 MoverPoints[2] = { Capsule.center1, Capsule.center2 };
	bool         bHit           = false;
	for (const b2ShapeId Shape : Candidates)
	{
		const uint64 Flags = reinterpret_cast<uint64>(b2Shape_GetUserData(Shape));
		const uint32 Body  = Impl->BodyOf(Shape);
		if (b2Shape_IsSensor(Shape) || IsFilteredProxy(Flags, Mover.bCollideCharacters) || Body == InvalidBody ||
		    (Mover.IgnoreUserData != 0 && Impl->Bodies[Body].UserData == Mover.IgnoreUserData))
		{
			continue;
		}
		const bool bOneWay = (Flags & ShapeFlagOneWay) != 0;
		if (bOneWay && Mover.bIgnoreOneWay)
		{
			continue;
		}
		b2ShapeCastPairInput Input{};
		if (!MakeWorldShapeProxy(Shape, Input.proxyA))
		{
			continue;
		}
		Input.proxyB       = b2MakeProxy(MoverPoints, 2, Capsule.radius);
		Input.transformA   = b2Transform_identity;
		Input.transformB   = b2Transform_identity;
		Input.translationB = Move;
		Input.maxFraction  = OutFraction;
		Input.canEncroach  = true;
		const b2CastOutput Output = b2ShapeCast(&Input);
		if (!Output.hit || Output.fraction <= 0.0f || Output.fraction >= OutFraction)
		{
			continue; // 빗나감 / 처음부터 겹침(무시 — Box2D 규칙) / 더 먼 것
		}
		const FVector2    Normal(Output.normal.x, Output.normal.y); // 모양 → 이동기
		const b2BodyId    BodyId = Impl->Bodies[Body].Body;
		const EBodyType2D Type   = ToBodyType(b2Body_GetType(BodyId));
		if (bOneWay)
		{
			const FVector2 Up = GetBodyUp(BodyId);
			if (FVector2::Dot(Normal, Up) < OneWayUpCos || FVector2::Dot(Translation, Up) >= 0.0f)
			{
				continue; // 위에서 내려앉을 때만
			}
		}
		if (Type == EBodyType2D::Dynamic && Normal.Y < Mover.WalkableNormalY)
		{
			continue;
		}
		OutFraction        = Output.fraction;
		OutHit.Normal      = Normal;
		OutHit.Point       = FromB2(Output.point);
		OutHit.Penetration = 0.0f;
		OutHit.Body        = Body;
		OutHit.UserData    = Impl->Bodies[Body].UserData;
		OutHit.bOneWay     = bOneWay;
		OutHit.bCharacter  = (Flags & ShapeFlagMoverProxy) != 0;
		OutHit.BodyType    = Type;
		bHit               = true;
	}
	return bHit;
}

void FPhysics2DWorld::MoveMover(const FPhysics2DMover& Mover, const FVector2& Position, const FVector2& Delta, FPhysics2DMoveResult& OutResult) const
{
	OutResult          = {};
	OutResult.Position = Position + Delta;
	OutResult.Velocity = Mover.Velocity;
	if (!Impl->IsValid())
	{
		return;
	}
	const FVector2                      Target = Position + Delta;
	std::vector<FPhysics2DMoverContact> Contacts;
	std::vector<b2CollisionPlane>       Planes;
	FVector2                            Current = Position;
	for (int Iteration = 0; Iteration < MoverIterations; ++Iteration)
	{
		++OutResult.Iterations;
		CollideMover(Mover, Current, 0.0f, Contacts);
		Planes.clear();
		for (const FPhysics2DMoverContact& Contact : Contacts)
		{
			b2CollisionPlane Plane;
			FVector2         Normal = Contact.Normal;
			float            Depth  = Contact.Penetration;
			if (Mover.bSteepAsWall && Normal.Y > 0.0f && Normal.Y < Mover.WalkableNormalY && std::abs(Normal.X) > 1.0e-3f)
			{
				Depth  = Depth / std::abs(Normal.X); // 수평으로 빠져나올 깊이
				Normal = FVector2(Normal.X > 0.0f ? 1.0f : -1.0f, 0.0f);
			}
			else if (Contact.bCharacter && Normal.Y >= Mover.WalkableNormalY)
			{
				// 다른 캐릭터 위(둥근 캡슐 머리): 평평한 발판처럼 위로만 빠져나온다 (기울어진 법선으로 풀면 머리에서 조금씩 미끄러져 내려간다)
				Depth  = Depth / Normal.Y;
				Normal = FVector2(0.0f, 1.0f);
			}
			Plane.plane        = b2Plane{ b2Vec2{ Normal.X, Normal.Y }, Depth * CmToM };
			Plane.pushLimit    = FLT_MAX;
			Plane.push         = 0.0f;
			Plane.clipVelocity = Normal.Y < Mover.WalkableNormalY; // 바닥 면은 속도를 자르지 않는다 (오르막 수평 속도 유지)
			Planes.push_back(Plane);
		}
		const b2PlaneSolverResult Solved      = b2SolvePlanes(ToB2(Target - Current), Planes.data(), static_cast<int>(Planes.size()));
		const FVector2            Translation = FromB2(Solved.translation);
		float                     Fraction    = 1.0f;
		FPhysics2DMoverContact    Hit;
		CastMover(Mover, Current, Translation, Fraction, Hit);
		const FVector2 Step = Translation * Fraction;
		Current             = Current + Step;
		if (Step.LengthSquared() < MoverToleranceCm * MoverToleranceCm)
		{
			break;
		}
	}
	OutResult.Position   = Current;
	const b2Vec2 Clipped = b2ClipVector(ToB2(Mover.Velocity), Planes.data(), static_cast<int>(Planes.size()));
	OutResult.Velocity   = FromB2(Clipped);
	for (const b2CollisionPlane& Plane : Planes)
	{
		if (Plane.push > 0.0f && Plane.plane.normal.y < -0.5f)
		{
			OutResult.bHitCeiling = true;
		}
	}
}

FVector2 FPhysics2DWorld::GetPointVelocity(uint32 Body, const FVector2& Point) const
{
	const FImpl::FBodySlot* Slot = Impl->Find(Body);
	return Slot != nullptr ? FromB2(b2Body_GetWorldPointVelocity(Slot->Body, ToB2(Point))) : FVector2();
}
