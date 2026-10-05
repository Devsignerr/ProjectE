#include "Editor/NavMeshBaker.h"

#include "AI/AIComponents.h"
#include "Physics/PhysicsComponents.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Terrain.h"

namespace
{
	// 자기 또는 조상이 움직이는 물체(동적 강체, AI 에이전트, 스크립트가 움직일 수 있는 엔티티)면 바닥 후보가 아니다
	bool IsMovingObject(const FScene& Scene, FEntity Entity)
	{
		const FRegistry& Registry = Scene.GetRegistry();
		for (FEntity Current = Entity; Registry.IsValid(Current); Current = Scene.GetParent(Current))
		{
			const FRigidBodyComponent* Body = Registry.TryGet<FRigidBodyComponent>(Current);
			if ((Body && Body->MotionType == static_cast<int32>(EPhysicsMotionType::Dynamic)) || Registry.Has<FBehaviorTreeComponent>(Current) ||
			    Registry.Has<FNavAgentComponent>(Current) || Registry.Has<FScriptComponent>(Current))
			{
				return true;
			}
		}
		return false;
	}
} // namespace

FNavMeshBuildInput FNavMeshBaker::CollectInput(FScene& Scene, FResourceManager& Resources, uint32* OutMeshCount)
{
	FNavMeshBuildInput Input;
	uint32             MeshCount = 0;
	Scene.GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each(
		[&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			const FStaticMesh* Mesh = MeshComponent.bVisible ? Resources.GetMesh(MeshComponent.Mesh) : nullptr;
			if (Mesh == nullptr || Mesh->IsSkinned() || Mesh->GetCpuIndices().empty() || IsMovingObject(Scene, Entity))
			{
				return;
			}
			AppendMesh(Mesh->GetCpuPositions(), Mesh->GetCpuIndices(), Transform.WorldMatrix, Input);
			++MeshCount;
		});
	std::vector<FTerrainInstance> Terrains;
	GatherTerrains(Scene, Terrains);
	for (const FTerrainInstance& Terrain : Terrains)
	{
		if (Terrain.Component->bCollision && !IsMovingObject(Scene, Terrain.Entity))
		{
			AppendTerrain(*Terrain.Data, Terrain.Frame, Input);
			++MeshCount;
		}
	}
	if (OutMeshCount != nullptr)
	{
		*OutMeshCount = MeshCount;
	}
	return Input;
}

void FNavMeshBaker::AppendMesh(const std::vector<FVector3>& Positions, const std::vector<uint32>& Indices, const FMatrix4x4& World, FNavMeshBuildInput& Input)
{
	const bool   bFlip = World.Determinant() < 0.0f; // 거울상 스케일은 와인딩이 뒤집힌다
	const uint32 Base  = static_cast<uint32>(Input.Vertices.size());
	Input.Vertices.reserve(Input.Vertices.size() + Positions.size());
	for (const FVector3& Position : Positions)
	{
		Input.Vertices.push_back(World.TransformPosition(Position));
	}
	Input.Indices.reserve(Input.Indices.size() + Indices.size());
	for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
	{
		Input.Indices.push_back(Base + Indices[Index]);
		Input.Indices.push_back(Base + Indices[Index + (bFlip ? 2 : 1)]);
		Input.Indices.push_back(Base + Indices[Index + (bFlip ? 1 : 2)]);
	}
}

void FNavMeshBaker::AppendTerrain(const FTerrainData& Data, const FTerrainFrame& Frame, FNavMeshBuildInput& Input)
{
	if (!Data.IsValid())
	{
		return;
	}
	const int32  Resolution = static_cast<int32>(Data.Resolution);
	const uint32 Base       = static_cast<uint32>(Input.Vertices.size());
	Input.Vertices.reserve(Input.Vertices.size() + static_cast<size_t>(Resolution) * Resolution);
	for (int32 Y = 0; Y < Resolution; ++Y)
	{
		for (int32 X = 0; X < Resolution; ++X)
		{
			Input.Vertices.push_back(Frame.GridToWorld(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Data.GetHeight(X, Y))));
		}
	}
	// 격자 +X = 월드 +X, +Y = 월드 +Y (CellSize 양수) → (00, 10, 11)·(00, 11, 01)이 위를 향한다 (Cross(P1 - P0, P2 - P0).Z > 0)
	const bool bFlip = Frame.CellSize.X * Frame.CellSize.Y < 0.0f;
	Input.Indices.reserve(Input.Indices.size() + static_cast<size_t>(Resolution - 1) * (Resolution - 1) * 6);
	for (int32 Y = 0; Y + 1 < Resolution; ++Y)
	{
		for (int32 X = 0; X + 1 < Resolution; ++X)
		{
			const uint32 V00 = Base + static_cast<uint32>(Y * Resolution + X);
			const uint32 V10 = V00 + 1;
			const uint32 V01 = V00 + static_cast<uint32>(Resolution);
			const uint32 V11 = V01 + 1;
			const uint32 Tris[6] = { V00, V10, V11, V00, V11, V01 };
			for (int32 T = 0; T < 6; T += 3)
			{
				Input.Indices.push_back(Tris[T]);
				Input.Indices.push_back(Tris[T + (bFlip ? 2 : 1)]);
				Input.Indices.push_back(Tris[T + (bFlip ? 1 : 2)]);
			}
		}
	}
}

FNavMeshBaker::FResult FNavMeshBaker::Bake(FScene& Scene, FResourceManager& Resources, FNavMesh& OutNavMesh)
{
	FResult Result;
	FNavMeshBuildSettings Settings;
	Scene.GetRegistry().View<FNavMeshComponent>().Each([&](FEntity, FNavMeshComponent& Component) { Settings = Component.ToBuildSettings(); });

	const FNavMeshBuildInput Input = CollectInput(Scene, Resources, &Result.MeshCount);
	Result.TriangleCount           = static_cast<uint32>(Input.Indices.size() / 3);
	if (Input.Indices.empty())
	{
		Result.Error = "굽을 정적 메시가 없습니다";
		return Result;
	}
	FNavMesh Built;
	if (!Built.Build(Input, Settings, &Result.Error))
	{
		return Result;
	}
	Result.PolygonCount = Built.GetPolygonCount();
	Result.bSucceeded   = true;
	OutNavMesh          = std::move(Built);
	return Result;
}
