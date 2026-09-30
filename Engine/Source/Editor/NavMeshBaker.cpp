#include "Editor/NavMeshBaker.h"

#include "AI/AIComponents.h"
#include "Physics/PhysicsComponents.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

namespace
{
	// 자기 또는 조상이 움직이는 물체(동적 강체, AI 에이전트)면 바닥 후보가 아니다
	bool IsMovingObject(const FScene& Scene, FEntity Entity)
	{
		const FRegistry& Registry = Scene.GetRegistry();
		for (FEntity Current = Entity; Registry.IsValid(Current); Current = Scene.GetParent(Current))
		{
			const FRigidBodyComponent* Body = Registry.TryGet<FRigidBodyComponent>(Current);
			if ((Body && Body->MotionType == static_cast<int32>(EPhysicsMotionType::Dynamic)) || Registry.Has<FBehaviorTreeComponent>(Current) ||
			    Registry.Has<FNavAgentComponent>(Current))
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
