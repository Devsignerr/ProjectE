#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/TerrainCollision.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Foliage.h"
#include "Scene/Terrain.h"

#include <cmath>
#include <string>

// 지형 높이맵 충돌 (TerrainCollision.h): Jolt 높이장 축/행 매핑, 캐릭터가 위에 서기, 편집 후 갱신
namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	// 9x9 정점, 셀 100cm, 높이 1 단위 = 0.1cm, 엔티티 (0,0,0) 가운데 → 격자 (0,0) = 월드 (-400, -400)
	std::string MakeTerrainAsset(const char* Name)
	{
		const std::filesystem::path Path = FTestRegistry::GetTempDirectory() / (std::string(Name) + ".eterrain");
		const std::string           Asset = Path.generic_string();
		FTerrainLibrary::Get().Invalidate(Asset);
		FTerrainLibrary::Get().Create(Asset, 9);
		return Asset;
	}

	FEntity AddTerrain(FScene& Scene, const std::string& Asset)
	{
		const FEntity      Entity  = Scene.CreateEntity("Terrain");
		FTerrainComponent& Terrain = Scene.GetRegistry().Emplace<FTerrainComponent>(Entity);
		Terrain.Asset              = Asset;
		Terrain.Size               = FVector2(800.0f, 800.0f);
		Terrain.HeightRange        = 6553.5f;
		return Entity;
	}

	float RayDownZ(FPhysicsSystem& Physics, float X, float Y)
	{
		FPhysicsHit Hit;
		if (!Physics.Raycast(FVector3(X, Y, 5000.0f), FVector3(0.0f, 0.0f, -1.0f), 20000.0f, Hit))
		{
			return -1.0e9f;
		}
		return Hit.Position.Z;
	}
} // namespace

E_TEST(TerrainCollision_AxisAndRowMapping)
{
	const std::string Asset = MakeTerrainAsset("AxisMapping");
	FTerrainData&     Data  = *FTerrainLibrary::Get().Find(Asset);
	// 일부러 비대칭인 봉우리: 격자 (GridX = 6, GridY = 2) → 월드 (200, -200)
	Data.Heights[2 * 9 + 6] = 32768 + 5000; // +500cm
	Data.MarkChanged(FTerrainRect::Full(9));

	FScene        Scene;
	const FEntity Terrain = AddTerrain(Scene, Asset);
	Scene.UpdateTransforms();
	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);

	FPhysicsHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector3(200.0f, -200.0f, 5000.0f), FVector3(0.0f, 0.0f, -1.0f), 20000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Terrain);
	E_EXPECT_NEAR(Hit.Position.Z, 500.0f, 1.0f);
	// 전치/뒤집힘이면 봉우리가 (−200, 200) 또는 (200, 200)에 나온다
	E_EXPECT_NEAR(RayDownZ(Physics, -200.0f, 200.0f), 0.0f, 1.0f);
	E_EXPECT_NEAR(RayDownZ(Physics, 200.0f, 200.0f), 0.0f, 1.0f);
	E_EXPECT_NEAR(RayDownZ(Physics, -200.0f, -200.0f), 0.0f, 1.0f);
	// 셀 안 높이도 TerrainMath와 같은 삼각형 분할
	const FTerrainFrame TerrainFrame = FTerrainFrame::Make(FVector3(), Scene.GetRegistry().Get<FTerrainComponent>(Terrain), 9);
	for (const FVector2 Point : { FVector2(150.0f, -170.0f), FVector2(240.0f, -230.0f), FVector2(170.0f, -150.0f) })
	{
		E_EXPECT_NEAR(RayDownZ(Physics, Point.X, Point.Y), TerrainMath::SampleWorldHeight(Data, TerrainFrame, Point.X, Point.Y), 1.0f);
	}
	Physics.End();
}

E_TEST(TerrainCollision_CharacterStandsAndUpdatesAfterEdit)
{
	const std::string Asset = MakeTerrainAsset("CharacterStand");
	FScene            Scene;
	AddTerrain(Scene, Asset);
	const FEntity Character = Scene.CreateEntity("Character");
	Scene.GetTransform(Character).Position = FVector3(0.0f, 0.0f, 300.0f);
	Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Character);
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	for (int32 Index = 0; Index < 120; ++Index)
	{
		FCharacterMove Move;
		Move.DeltaSeconds = Frame;
		Physics.SimulateCharacter(Scene, Character, Move);
		Physics.Update(Scene, Frame);
		Scene.UpdateTransforms();
	}
	// 바닥 z = 0, 캡슐 중심 = 반지름 35 + 원기둥 절반 55
	E_EXPECT_TRUE(Physics.IsGrounded(Character));
	E_EXPECT_NEAR(Scene.GetTransform(Character).Position.Z, 90.0f, 3.0f);

	// 지형을 1m 올리면 다음 Update에서 바디가 바뀐다
	FTerrainData& Data = *FTerrainLibrary::Get().Find(Asset);
	for (uint16& Height : Data.Heights)
	{
		Height = 32768 + 1000;
	}
	Data.MarkChanged(FTerrainRect::Full(9));
	Physics.Update(Scene, Frame);
	E_EXPECT_NEAR(RayDownZ(Physics, 300.0f, 300.0f), 100.0f, 1.0f); // 캐릭터 캡슐을 피해서
	Physics.End();
}

E_TEST(TerrainCollision_DisabledHasNoBody)
{
	const std::string Asset = MakeTerrainAsset("Disabled");
	FScene            Scene;
	const FEntity     Terrain = AddTerrain(Scene, Asset);
	Scene.GetRegistry().Get<FTerrainComponent>(Terrain).bCollision = false;
	Scene.UpdateTransforms();
	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	FPhysicsHit Hit;
	E_EXPECT_FALSE(Physics.Raycast(FVector3(0.0f, 0.0f, 5000.0f), FVector3(0.0f, 0.0f, -1.0f), 20000.0f, Hit));
	Physics.End();
}

E_TEST(FoliageCollision_TreeCapsulesInOneBody)
{
	const std::filesystem::path Path  = FTestRegistry::GetTempDirectory() / "Trees.efoliage";
	const std::string           Asset = Path.generic_string();
	FFoliageAsset               Initial;
	FFoliageType&               Tree = Initial.Types.emplace_back();
	Tree.bCollision                  = true;
	Tree.CollisionRadius             = 20.0f;
	Tree.CollisionHeight             = 400.0f;
	FFoliageType& Grass              = Initial.Types.emplace_back();
	Grass.bCollision                 = false;
	Initial.EnsureInstanceLists();
	for (const float X : { 300.0f, 600.0f })
	{
		FFoliageInstance& Instance = Initial.Instances[0].emplace_back();
		Instance.Position          = FVector3(X, 0.0f, 0.0f);
	}
	Initial.Instances[1].push_back({ FVector3(-300.0f, 0.0f, 0.0f), 0.0f, 1.0f, FVector3::UpVector });
	FFoliageLibrary::Get().Invalidate(Asset);
	E_EXPECT_TRUE(FFoliageLibrary::Get().Create(Asset, Initial) != nullptr);

	FScene        Scene;
	const FEntity Foliage = Scene.CreateEntity("Foliage");
	Scene.GetRegistry().Emplace<FFoliageComponent>(Foliage).Asset = Asset;
	Scene.UpdateTransforms();
	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	E_EXPECT_EQ(Physics.GetBodyCount(), 1u); // 캡슐 2개 = 바디 1개

	FPhysicsHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector3(-1000.0f, 0.0f, 150.0f), FVector3(1.0f, 0.0f, 0.0f), 5000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Foliage);
	E_EXPECT_NEAR(Hit.Position.X, 280.0f, 1.0f); // 첫 나무 (풀은 충돌 없음)
	// 캡슐 위(높이 400cm)를 넘으면 맞지 않는다
	E_EXPECT_FALSE(Physics.Raycast(FVector3(-1000.0f, 0.0f, 450.0f), FVector3(1.0f, 0.0f, 0.0f), 5000.0f, Hit));
	Physics.End();
}
