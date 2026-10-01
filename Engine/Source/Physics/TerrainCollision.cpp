#include "Physics/TerrainCollision.h"

#include "Core/Math/Units.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Terrain.h"

#pragma warning(push, 0)
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#pragma warning(pop)

#include <algorithm>
#include <vector>

namespace
{
	// PhysicsWorld.cpp의 ObjectLayers::NonMoving과 같은 값 (정적 바디 층)
	constexpr JPH::ObjectLayer TerrainObjectLayer = 0;

	// 엔진 축 순환 회전: Jolt X → 엔진 Y, Jolt Y(높이) → 엔진 Z, Jolt Z → 엔진 X (축 (1,1,1) 120°)
	const JPH::Quat TerrainRotation(0.5f, 0.5f, 0.5f, 0.5f);
} // namespace

void FTerrainCollision::BuildSamples(const FTerrainData& Data, float HeightScaleMeters, std::vector<float>& OutSamples)
{
	const uint32 Resolution = Data.Resolution;
	OutSamples.resize(static_cast<size_t>(Resolution) * Resolution);
	// Jolt 표본 (x, y)는 y * N + x. 엔진 격자 (GridX = y, GridY = x) = Heights[x * N + y] (전치)
	for (uint32 JoltY = 0; JoltY < Resolution; ++JoltY)
	{
		for (uint32 JoltX = 0; JoltX < Resolution; ++JoltX)
		{
			OutSamples[static_cast<size_t>(JoltY) * Resolution + JoltX] =
				static_cast<float>(Data.Heights[static_cast<size_t>(JoltX) * Resolution + JoltY]) * HeightScaleMeters;
		}
	}
}

void FTerrainCollision::Sync(FScene& Scene, FPhysicsWorld& World)
{
	++Frame;
	std::vector<FTerrainInstance> Terrains;
	GatherTerrains(Scene, Terrains);

	JPH::BodyInterface& BodyInterface = World.GetJoltBodyInterface();
	for (const FTerrainInstance& Terrain : Terrains)
	{
		if (!Terrain.Component->bCollision)
		{
			continue;
		}
		const FTerrainFrame& TerrainFrame = Terrain.Frame;
		const float          Key[6] = { TerrainFrame.Origin.X, TerrainFrame.Origin.Y, TerrainFrame.Origin.Z, Terrain.Component->Size.X, Terrain.Component->Size.Y,
		                                Terrain.Component->HeightRange };

		FBody& Entry   = Bodies[Terrain.Entity];
		Entry.LastSeen = Frame;
		// 실패한 생성도 같은 입력이면 다시 시도하지 않는다 (매 프레임 오류 반복 방지)
		const bool bSame = Entry.Data == Terrain.Data && Entry.ChangeCounter == Terrain.Data->ChangeCounter &&
		                   std::equal(std::begin(Key), std::end(Key), std::begin(Entry.Key));
		if (bSame)
		{
			continue;
		}
		if (Entry.Body != ~0u)
		{
			World.DestroyBody(Entry.Body);
			Entry.Body = ~0u;
		}
		Entry.Data          = Terrain.Data;
		Entry.ChangeCounter = Terrain.Data->ChangeCounter;
		std::copy(std::begin(Key), std::end(Key), std::begin(Entry.Key));

		std::vector<float> Samples;
		BuildSamples(*Terrain.Data, TerrainFrame.HeightScale * FUnits::UnitsToMeters, Samples);
		const JPH::Vec3 Scale(TerrainFrame.CellSize.Y * FUnits::UnitsToMeters, 1.0f, TerrainFrame.CellSize.X * FUnits::UnitsToMeters);
		JPH::HeightFieldShapeSettings Settings(Samples.data(), JPH::Vec3::sZero(), Scale, Terrain.Data->Resolution);
		Settings.mBlockSize     = 4;
		Settings.mBitsPerSample = 16;
		const JPH::ShapeSettings::ShapeResult Result = Settings.Create();
		if (Result.HasError())
		{
			E_LOG(LogPhysics, Error, "지형 충돌 생성 실패: {}", Result.GetError().c_str());
			continue;
		}
		const JPH::RVec3 Position(TerrainFrame.Origin.X * FUnits::UnitsToMeters, TerrainFrame.Origin.Y * FUnits::UnitsToMeters,
		                          TerrainFrame.Origin.Z * FUnits::UnitsToMeters);
		JPH::BodyCreationSettings BodySettings(Result.Get(), Position, TerrainRotation, JPH::EMotionType::Static, TerrainObjectLayer);
		BodySettings.mFriction = 0.6f;
		BodySettings.mUserData = Terrain.Entity.ToId();
		const JPH::BodyID Id   = BodyInterface.CreateAndAddBody(BodySettings, JPH::EActivation::DontActivate);
		if (Id.IsInvalid())
		{
			E_LOG(LogPhysics, Error, "지형 충돌 바디 생성 실패 (최대 바디 수 초과?)");
			continue;
		}
		Entry.Body = Id.GetIndexAndSequenceNumber();
		E_LOG(LogPhysics, Log, "지형 충돌: {}x{} 높이장", Terrain.Data->Resolution, Terrain.Data->Resolution);
	}

	// 사라진 지형 / 충돌 끔
	for (auto It = Bodies.begin(); It != Bodies.end();)
	{
		if (It->second.LastSeen != Frame)
		{
			if (It->second.Body != ~0u)
			{
				World.DestroyBody(It->second.Body);
			}
			It = Bodies.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FTerrainCollision::Clear(FPhysicsWorld* World)
{
	if (World != nullptr)
	{
		for (const auto& [Entity, Entry] : Bodies)
		{
			World->DestroyBody(Entry.Body); // 무효 ID는 무시된다
		}
	}
	Bodies.clear();
}
