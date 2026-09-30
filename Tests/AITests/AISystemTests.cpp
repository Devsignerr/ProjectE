#include "AI/AIComponents.h"
#include "AI/AIModule.h"
#include "AI/AISystem.h"
#include "AI/BehaviorTree/BehaviorTreeAsset.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"

#include <cmath>
#include <filesystem>

namespace
{
	constexpr float Dt = 0.1f;

	std::filesystem::path MakeContentDirectory(const wchar_t* Name)
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"AISystem" / Name;
		std::filesystem::create_directories(Directory / L"AI");
		return Directory;
	}

	FBTNodeDesc Node(const char* Type, std::vector<FBTParam> Params = {}, std::vector<FBTNodeDesc> Children = {})
	{
		FBTNodeDesc Desc;
		Desc.Type     = Type;
		Desc.Params   = std::move(Params);
		Desc.Children = std::move(Children);
		return Desc;
	}

	FBTParam Param(const char* Name, FBTParamValue Value) { return { Name, std::move(Value) }; }

	// Sequence [ Goal 설정 → MoveTo Goal → Done = true ]. 끝나면 루트가 다시 시작하므로 Done만 본다
	void WriteMoveTree(const std::filesystem::path& File, const char* GoalText)
	{
		FBehaviorTreeAsset Asset;
		Asset.BlackboardKeys = { { "Goal", EBlackboardKeyType::Vector }, { "Done", EBlackboardKeyType::Bool } };
		Asset.Root           = Node("Sequence", {},
			{
				Node("SetBlackboard", { Param("Key", std::string("Goal")), Param("Value", std::string(GoalText)) }),
				Node("MoveTo", { Param("TargetKey", std::string("Goal")) }),
				Node("SetBlackboard", { Param("Key", std::string("Done")), Param("Value", std::string("true")) }),
				Node("Wait", { Param("WaitTime", 1000.0f) }), // 루트 재시작으로 다시 움직이지 않게 붙잡아 둔다
			});
		E_EXPECT_TRUE(Asset.SaveToFile(File));
	}

	FEntity MakeAgent(FScene& Scene, const char* TreeAsset, const FVector3& Position)
	{
		const FEntity Agent                 = Scene.CreateEntity("Agent");
		Scene.GetTransform(Agent).Position = Position;
		Scene.GetRegistry().Emplace<FBehaviorTreeComponent>(Agent).Asset = TreeAsset;
		Scene.GetRegistry().Emplace<FNavAgentComponent>(Agent).MaxSpeed  = 300.0f;
		Scene.UpdateTransforms();
		return Agent;
	}

	// AI 갱신 후 트랜스폼 갱신 (FGameWorld 순서에서 물리 없이)
	void Step(FAISystem& AI, FScene& Scene, int32 Frames)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			AI.Update(Scene, Dt);
			Scene.UpdateTransforms();
		}
	}

	bool IsDone(FAISystem& AI, FEntity Agent)
	{
		FBehaviorTreeInstance* Tree = AI.FindTree(Agent);
		return Tree != nullptr && Tree->GetBlackboard().Get<bool>("Done").value_or(false);
	}

	// ---- 내비메시 지오메트리 (NavMeshTests와 같은 규약: Cross(P1 - P0, P2 - P0)가 앞면)
	void AddQuad(FNavMeshBuildInput& Input, const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& D, const FVector3& Normal)
	{
		const uint32 Base = static_cast<uint32>(Input.Vertices.size());
		Input.Vertices.insert(Input.Vertices.end(), { A, B, C, D });
		if (FVector3::Dot(FVector3::Cross(B - A, C - A), Normal) > 0.0f)
		{
			Input.Indices.insert(Input.Indices.end(), { Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
		}
		else
		{
			Input.Indices.insert(Input.Indices.end(), { Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
		}
	}

	constexpr float WallHalfX = 50.0f;
	constexpr float WallHalfY = 600.0f;

	FNavMesh BuildFloorWithWall()
	{
		FNavMeshBuildInput Input;
		AddQuad(Input, FVector3(-1000.0f, -1000.0f, 0.0f), FVector3(1000.0f, -1000.0f, 0.0f), FVector3(1000.0f, 1000.0f, 0.0f),
		        FVector3(-1000.0f, 1000.0f, 0.0f), FVector3::UpVector);
		const FVector3 Min(-WallHalfX, -WallHalfY, 0.0f);
		const FVector3 Max(WallHalfX, WallHalfY, 300.0f);
		const FVector3 P000(Min.X, Min.Y, Min.Z), P100(Max.X, Min.Y, Min.Z), P110(Max.X, Max.Y, Min.Z), P010(Min.X, Max.Y, Min.Z);
		const FVector3 P001(Min.X, Min.Y, Max.Z), P101(Max.X, Min.Y, Max.Z), P111(Max.X, Max.Y, Max.Z), P011(Min.X, Max.Y, Max.Z);
		AddQuad(Input, P001, P101, P111, P011, FVector3(0.0f, 0.0f, 1.0f));
		AddQuad(Input, P000, P010, P011, P001, FVector3(-1.0f, 0.0f, 0.0f));
		AddQuad(Input, P100, P110, P111, P101, FVector3(1.0f, 0.0f, 0.0f));
		AddQuad(Input, P000, P100, P101, P001, FVector3(0.0f, -1.0f, 0.0f));
		AddQuad(Input, P010, P110, P111, P011, FVector3(0.0f, 1.0f, 0.0f));

		FNavMesh    NavMesh;
		std::string Error;
		E_EXPECT_TRUE(NavMesh.Build(Input, FNavMeshBuildSettings{}, &Error));
		return NavMesh;
	}
} // namespace

E_TEST(AISystem_TreeMovesAgentStraightWithoutNavMesh)
{
	RegisterAITypes();
	const std::filesystem::path Content = MakeContentDirectory(L"Straight");
	WriteMoveTree(Content / L"AI" / L"Move.ebt", "500,0,0");

	FScene        Scene;
	const FEntity Agent = MakeAgent(Scene, "AI/Move.ebt", FVector3::ZeroVector);

	FAISystem AI;
	AI.SetContentDirectory(Content);
	AI.Begin(Scene);
	E_EXPECT_TRUE(AI.FindTree(Agent) != nullptr);

	// 300cm/s × 0.1초 → 약 17프레임에 도착 (도착 반경 20cm)
	Step(AI, Scene, 10);
	E_EXPECT_FALSE(IsDone(AI, Agent));
	E_EXPECT_TRUE(AI.GetMoveStatus(Agent) == EAIMoveStatus::Moving);
	E_EXPECT_NEAR(Scene.GetTransform(Agent).GetWorldPosition().X, 300.0f, 1.0f);

	Step(AI, Scene, 10);
	E_EXPECT_TRUE(IsDone(AI, Agent));
	const FVector3 Position = Scene.GetTransform(Agent).GetWorldPosition();
	E_EXPECT_TRUE(std::abs(Position.X - 500.0f) <= 20.0f + 1.0e-3f);
	E_EXPECT_NEAR(Position.Y, 0.0f, 1.0e-3f);

	// 이동 방향(+X)을 바라본다
	E_EXPECT_NEAR(Scene.GetTransform(Agent).WorldMatrix.M[0][0], 1.0f, 1.0e-3f);
	AI.End();
	E_EXPECT_TRUE(AI.FindTree(Agent) == nullptr);
}

E_TEST(AISystem_MoveGoesAroundWallOnNavMesh)
{
	RegisterAITypes();
	const std::filesystem::path Content = MakeContentDirectory(L"NavMesh");
	WriteMoveTree(Content / L"AI" / L"Move.ebt", "400,0,0");

	FScene        Scene;
	const FEntity Agent = MakeAgent(Scene, "AI/Move.ebt", FVector3(-400.0f, 0.0f, 0.0f));

	// 내비메시는 Begin 전에 지정한다 (Begin에서 시작한 트리의 첫 MoveTo가 경로를 찾는다)
	FAISystem AI;
	AI.SetContentDirectory(Content);
	AI.SetNavMesh(BuildFloorWithWall());
	AI.Begin(Scene);
	E_EXPECT_TRUE(AI.GetNavMesh() != nullptr);
	E_EXPECT_TRUE(AI.GetMovePath(Agent) != nullptr && AI.GetMovePath(Agent)->size() > 2u); // 벽을 도는 경로

	bool bEnteredWall = false;
	for (int32 Frame = 0; Frame < 200 && !IsDone(AI, Agent); ++Frame)
	{
		Step(AI, Scene, 1);
		const FVector3 P = Scene.GetTransform(Agent).GetWorldPosition();
		bEnteredWall     = bEnteredWall || (std::abs(P.X) < WallHalfX && std::abs(P.Y) < WallHalfY);
	}
	E_EXPECT_TRUE(IsDone(AI, Agent));
	E_EXPECT_FALSE(bEnteredWall);
	const FVector3 Position = Scene.GetTransform(Agent).GetWorldPosition();
	E_EXPECT_TRUE(std::hypot(Position.X - 400.0f, Position.Y) <= 20.0f + 1.0f);
}

E_TEST(AISystem_RequestMoveStatusAndStop)
{
	FScene        Scene;
	const FEntity Mover = Scene.CreateEntity("Mover");
	Scene.UpdateTransforms();

	FAISystem AI;
	E_EXPECT_TRUE(AI.RequestMove(Mover, FVector3(100.0f, 0.0f, 0.0f)) == EAIMoveStatus::Failed); // Begin 전
	AI.Begin(Scene);

	// 이미 도착 반경 안이면 바로 성공, 트리 없이도 이동 요청을 받는다
	E_EXPECT_TRUE(AI.RequestMove(Mover, FVector3(10.0f, 0.0f, 0.0f)) == EAIMoveStatus::Succeeded);
	E_EXPECT_TRUE(AI.RequestMove(Mover, FVector3(1000.0f, 0.0f, 0.0f)) == EAIMoveStatus::Moving);
	E_EXPECT_TRUE(AI.GetMovePath(Mover) != nullptr);
	Step(AI, Scene, 2);
	AI.StopMove(Mover);
	E_EXPECT_TRUE(AI.GetMoveStatus(Mover) == EAIMoveStatus::Idle);
	const float Stopped = Scene.GetTransform(Mover).GetWorldPosition().X;
	E_EXPECT_NEAR(Stopped, 60.0f, 1.0f);
	Step(AI, Scene, 2);
	E_EXPECT_NEAR(Scene.GetTransform(Mover).GetWorldPosition().X, Stopped, 1.0e-3f);
}

E_TEST(AISystem_PhysicsHookReceivesVelocity)
{
	FScene        Scene;
	const FEntity Body = Scene.CreateEntity("Body");
	Scene.UpdateTransforms();

	FVector3 LastVelocity(1.0f, 1.0f, 1.0f);
	int32    Calls = 0;
	FAISystem AI;
	AI.SetMovementHooks({ [&](FEntity Entity, const FVector3& Velocity) {
		E_EXPECT_TRUE(Entity == Body);
		LastVelocity = Velocity;
		++Calls;
		return true; // 물리가 맡는다 → 트랜스폼은 그대로
	} });
	AI.Begin(Scene);
	AI.RequestMove(Body, FVector3(0.0f, 1000.0f, 0.0f));
	Step(AI, Scene, 3);
	E_EXPECT_EQ(Calls, 3);
	E_EXPECT_EQUALS(LastVelocity, FVector3(0.0f, 300.0f, 0.0f), 1.0e-3f);
	E_EXPECT_EQUALS(Scene.GetTransform(Body).GetWorldPosition(), FVector3::ZeroVector, 1.0e-3f);

	AI.StopMove(Body); // 멈추면 수평 속도 0을 넘긴다
	E_EXPECT_EQ(Calls, 4);
	E_EXPECT_EQUALS(LastVelocity, FVector3::ZeroVector, 1.0e-3f);
}

E_TEST(AISystem_SyncsDestroyedEntitiesAndRemovedComponents)
{
	RegisterAITypes();
	const std::filesystem::path Content = MakeContentDirectory(L"Sync");
	WriteMoveTree(Content / L"AI" / L"Move.ebt", "500,0,0");

	FScene        Scene;
	const FEntity A = MakeAgent(Scene, "AI/Move.ebt", FVector3::ZeroVector);
	const FEntity B = MakeAgent(Scene, "AI/Move.ebt", FVector3(0.0f, 300.0f, 0.0f));

	FAISystem AI;
	AI.SetContentDirectory(Content);
	AI.Begin(Scene);
	Step(AI, Scene, 1);
	E_EXPECT_TRUE(AI.FindTree(A) != nullptr && AI.FindTree(B) != nullptr);

	Scene.DestroyEntity(A);
	Scene.GetRegistry().Remove<FBehaviorTreeComponent>(B);
	Step(AI, Scene, 1);
	E_EXPECT_TRUE(AI.FindTree(A) == nullptr);
	E_EXPECT_TRUE(AI.FindTree(B) == nullptr);

	// 플레이 중 새로 생긴 엔티티(스크립트 생성 등)도 다음 갱신에 트리가 생긴다
	const FEntity C = MakeAgent(Scene, "AI/Move.ebt", FVector3::ZeroVector);
	Step(AI, Scene, 1);
	E_EXPECT_TRUE(AI.FindTree(C) != nullptr);

	// 없는 에셋은 한 번만 실패하고 트리 없이 남는다
	const FEntity Broken = MakeAgent(Scene, "AI/Missing.ebt", FVector3::ZeroVector);
	Step(AI, Scene, 3);
	E_EXPECT_TRUE(AI.FindTree(Broken) == nullptr);
}

E_TEST(AISystem_ReloadRestartsTreesUsingAsset)
{
	RegisterAITypes();
	const std::filesystem::path Content = MakeContentDirectory(L"Reload");
	const std::filesystem::path File    = Content / L"AI" / L"Move.ebt";
	WriteMoveTree(File, "500,0,0");

	FScene        Scene;
	const FEntity Agent = MakeAgent(Scene, "AI/Move.ebt", FVector3::ZeroVector);

	FAISystem AI;
	AI.SetContentDirectory(Content);
	AI.Begin(Scene);
	Step(AI, Scene, 30);
	E_EXPECT_TRUE(IsDone(AI, Agent));

	// 목표를 바꿔 저장 → 다시 읽기: 새 트리가 처음부터 돈다 (블랙보드 초기화)
	WriteMoveTree(File, "500,300,0");
	AI.ReloadBehaviorTree("AI/Move.ebt");
	E_EXPECT_FALSE(IsDone(AI, Agent));
	Step(AI, Scene, 30);
	E_EXPECT_TRUE(IsDone(AI, Agent));
	E_EXPECT_TRUE(std::abs(Scene.GetTransform(Agent).GetWorldPosition().Y - 300.0f) <= 21.0f);
}

E_TEST(AISystem_TurnTowardsRespectsSpeedAndTolerance)
{
	FScene        Scene;
	const FEntity Entity = Scene.CreateEntity("Turner");
	Scene.GetTransform(Entity).Position = FVector3(100.0f, 0.0f, 0.0f);
	Scene.UpdateTransforms();

	// +Y(오른쪽, Yaw +90) 쪽 점을 45도씩 → 두 번에 도달
	const FVector3 Right(100.0f, 500.0f, 0.0f);
	E_EXPECT_FALSE(FAISystem::TurnTowards(Scene, Entity, Right, 45.0f, 1.0f));
	Scene.UpdateTransforms();
	E_EXPECT_TRUE(FAISystem::TurnTowards(Scene, Entity, Right, 45.0f, 1.0f));
	Scene.UpdateTransforms();
	const FMatrix4x4& World = Scene.GetTransform(Entity).WorldMatrix;
	E_EXPECT_NEAR(World.M[0][0], 0.0f, 1.0e-3f);
	E_EXPECT_NEAR(World.M[0][1], 1.0f, 1.0e-3f);
	E_EXPECT_EQUALS(Scene.GetTransform(Entity).GetWorldPosition(), FVector3(100.0f, 0.0f, 0.0f), 1.0e-3f);

	// 부모가 돌아가 있어도 월드 기준으로 맞춘다
	const FEntity Parent = Scene.CreateEntity("Parent");
	Scene.GetTransform(Parent).Rotation = FQuat::FromEuler(0.0f, 30.0f, 0.0f);
	const FEntity Child = Scene.CreateEntity("Child");
	Scene.SetParent(Child, Parent);
	Scene.UpdateTransforms();
	E_EXPECT_TRUE(FAISystem::TurnTowards(Scene, Child, FVector3(-100.0f, 0.0f, 0.0f), 360.0f, 0.5f));
	Scene.UpdateTransforms();
	E_EXPECT_NEAR(Scene.GetTransform(Child).WorldMatrix.M[0][0], -1.0f, 1.0e-3f);
}
