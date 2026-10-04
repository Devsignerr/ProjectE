// 타일맵 충돌 → 2D 물리 (FPhysics2DSystem::BuildTilemapShapes — Physics/Physics2DSystem.h 머리 주석): 타일 위 정지, 원웨이 타일,
// 타일을 지우면 바디를 다시 만들어 떨어짐, TileData 문자열이 바뀌어도(복제·Undo) 다시 만듦. 타일셋은 임시 폴더에 파일로 쓴다
#include "Core/Testing/TestFramework.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DSystem.h"
#include "Physics/PhysicsWorld.h" // LogPhysics
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	// 타일 0 = Full, 1 = Full + OneWay (16px, 셀 크기는 컴포넌트가 100cm로 정한다)
	std::string WriteTileset()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectETilemap2DPhysicsTests";
		std::filesystem::create_directories(Directory);
		FTilesetAsset Tileset;
		Tileset.Texture       = "Tiles.png";
		Tileset.TextureWidth  = 32;
		Tileset.TextureHeight = 16;
		FTileDefinition Ground;
		Ground.Id        = 0;
		Ground.Collision = ETileCollision::Full;
		FTileDefinition Platform;
		Platform.Id        = 1;
		Platform.Collision = ETileCollision::Full;
		Platform.bOneWay   = true;
		Tileset.Tiles      = { Ground, Platform };
		const std::string Path = (Directory / L"PhysicsTiles.etileset").generic_string();
		E_EXPECT_TRUE(FSprite2DLibrary::Get().SaveTileset(Path, Tileset));
		return Path;
	}

	FEntity AddTilemap(FScene& Scene, const std::string& TilesetPath)
	{
		const FEntity      Entity  = Scene.CreateEntity("Tilemap");
		FTilemapComponent& Tilemap = Scene.GetRegistry().Emplace<FTilemapComponent>(Entity);
		Tilemap.Tileset            = TilesetPath;
		Tilemap.CellSize           = FVector2(100.0f, 100.0f);
		return Entity;
	}

	void FillRow(FTilemapComponent& Tilemap, int32 MinX, int32 MaxX, int32 Y, int32 TileId)
	{
		Sprite2DRuntime::GetTilemapData(Tilemap);
		Tilemap.Runtime.Data.FillRect(FTileRect::FromCorners(MinX, Y, MaxX, Y), TileCell::Make(TileId));
		Sprite2DRuntime::CommitTilemapData(Tilemap);
	}

	FEntity AddBox(FScene& Scene, const char* Name, const FVector3& Position)
	{
		const FEntity Box = Scene.CreateEntity(Name);
		Scene.GetTransform(Box).Position = Position;
		Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Box).Size = FVector2(100.0f, 100.0f);
		Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Box).bFixedRotation = true;
		return Box;
	}

	void Simulate(FPhysics2DSystem& Physics, FScene& Scene, float Seconds)
	{
		const int32 Frames = static_cast<int32>(std::lround(Seconds / Frame));
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
		}
	}
} // namespace

// 타일 줄(y = -1, 위 = Z 0) 위에 떨어진 상자가 멈춘다. 셀 (0,0) 왼쪽 아래 = 엔티티 원점이라 엔티티를 옮기면 바닥도 옮겨진다
E_TEST(Tilemap2DPhysics_BoxRestsOnTiles)
{
	const std::string TilesetPath = WriteTileset();
	FScene            Scene;
	const FEntity     Map = AddTilemap(Scene, TilesetPath);
	FillRow(Scene.GetRegistry().Get<FTilemapComponent>(Map), -5, 5, -1, 0);
	const FEntity Box = AddBox(Scene, "Box", FVector3(50.0f, 30.0f, 300.0f));
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 2.0f);
	E_EXPECT_TRUE(Physics.HasBody(Map));
	E_EXPECT_FALSE(Physics.IsDynamicBody(Map)); // 강체 없음 = 정적
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, 50.0f, 1.5f);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Y, 30.0f, 1.0e-4f);

	// 정적 타일맵을 내리면 (순간이동, 바디는 그대로) 상자가 따라 내려가 새 바닥에 선다
	const uint32 BodiesBefore = Physics.GetBodyCount();
	Scene.GetTransform(Map).Position = FVector3(0.0f, 0.0f, -100.0f);
	Scene.UpdateTransforms();
	Simulate(Physics, Scene, 1.5f);
	E_EXPECT_EQ(Physics.GetBodyCount(), BodiesBefore);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, -50.0f, 1.5f);
	Physics.End();
	E_EXPECT_FALSE(Physics.HasBody(Map));
}

// 원웨이 타일: 아래에서 위로 올라가는 상자는 통과하고, 떨어질 때는 위에 착지한다
E_TEST(Tilemap2DPhysics_OneWayTilesPassFromBelow)
{
	const std::string  TilesetPath = WriteTileset();
	FScene             Scene;
	const FEntity      Map     = AddTilemap(Scene, TilesetPath);
	FTilemapComponent& Tilemap = Scene.GetRegistry().Get<FTilemapComponent>(Map);
	FillRow(Tilemap, -5, 5, -1, 0); // 바닥 (위 = 0)
	FillRow(Tilemap, -2, 2, 3, 1);  // 원웨이 발판 (Z 300 ~ 400)
	const FEntity Box = AddBox(Scene, "Box", FVector3(50.0f, 0.0f, 100.0f));
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	Physics.SetVelocity(Box, FVector2(0.0f, 1100.0f)); // 최고점 ≈ 100 + 1100²/(2·980) ≈ 717cm
	float MaxHeight = 0.0f;
	for (int32 Index = 0; Index < 180; ++Index)
	{
		Physics.Update(Scene, Frame);
		Scene.UpdateTransforms();
		MaxHeight = std::max(MaxHeight, Scene.GetTransform(Box).Position.Z);
	}
	E_EXPECT_TRUE(MaxHeight > 550.0f);                                   // 막히지 않고 통과
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, 400.0f + 50.0f, 2.0f); // 발판 위에 착지
	Physics.End();
}

// 타일을 지우면 (Commit → Revision) 다음 물리 갱신에 바디를 다시 만들어 상자가 떨어진다. 빈 맵이면 바디 없음.
// TileData 문자열을 직접 바꿔도 (복제·Undo 경로) 다시 디코딩해 충돌이 돌아온다
E_TEST(Tilemap2DPhysics_ErasedTileRebuildsBody)
{
	const std::string  TilesetPath = WriteTileset();
	FScene             Scene;
	const FEntity      Map     = AddTilemap(Scene, TilesetPath);
	FTilemapComponent& Tilemap = Scene.GetRegistry().Get<FTilemapComponent>(Map);
	FillRow(Tilemap, 0, 0, -1, 0); // 셀 하나 (X 0 ~ 100)
	const std::string OneTile = Tilemap.TileData;
	const FEntity     Box     = AddBox(Scene, "Box", FVector3(50.0f, 0.0f, 300.0f));
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 2.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, 50.0f, 1.5f);
	E_EXPECT_EQ(Physics.GetBodyCount(), 2u);

	Tilemap.Runtime.Data.Erase(0, -1);
	Sprite2DRuntime::CommitTilemapData(Tilemap);
	E_EXPECT_TRUE(Tilemap.TileData.empty());
	Simulate(Physics, Scene, 1.0f);
	E_EXPECT_FALSE(Physics.HasBody(Map));
	E_EXPECT_EQ(Physics.GetBodyCount(), 1u);
	E_EXPECT_TRUE(Scene.GetTransform(Box).Position.Z < -200.0f);

	// 복제처럼 문자열만 바뀜 → 다시 디코딩 → 바디 생성
	Tilemap.TileData                 = OneTile;
	Scene.GetTransform(Box).Position = FVector3(50.0f, 0.0f, 300.0f); // 순간이동
	Scene.UpdateTransforms();
	Simulate(Physics, Scene, 2.0f);
	E_EXPECT_TRUE(Physics.HasBody(Map));
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, 50.0f, 1.5f);

	// 충돌 끔 → 바디 없음
	Tilemap.bCollision = false;
	Physics.Update(Scene, Frame);
	E_EXPECT_FALSE(Physics.HasBody(Map));
	Physics.End();
}

// 이음매 고스트 충돌: Full 영역은 외곽선 닫힌 체인이라 병합 상자 경계(아래 줄이 짧아 윗면이 상자 두 개로 나뉘는 X = 500)를
// 마찰 없이 미끄러지는 상자/원이 튀거나 멈추지 않고 지나간다 (수평 속도 유지, 높이 일정). 비교로 같은 모양의 상자 콜라이더 두 개 바닥도 잰다(로그만)
E_TEST(Tilemap2DPhysics_SlidingAcrossSeamKeepsVelocity)
{
	const std::string TilesetPath = WriteTileset();
	struct FResult
	{
		float MinVelocityX = 1.0e9f;
		float MaxHeightError = 0.0f;
		float EndX = 0.0f;
	};
	// bTiles: 타일맵(체인) / 아니면 상자 콜라이더 둘 (A: X 0~500·Z 0~100, B: X 500~1000·Z 50~100 — 윗면 이음매 X 500)
	const auto Run = [&](bool bTiles, bool bCircle) {
		FScene Scene;
		if (bTiles)
		{
			const FEntity      Map     = AddTilemap(Scene, TilesetPath);
			FTilemapComponent& Tilemap = Scene.GetRegistry().Get<FTilemapComponent>(Map);
			Tilemap.CellSize           = FVector2(50.0f, 50.0f);
			Tilemap.Friction           = 0.0f;
			FillRow(Tilemap, 0, 9, 0, 0);  // 아래 줄 (짧음)
			FillRow(Tilemap, 0, 19, 1, 0); // 위 줄 (김) — 병합 상자면 X 500에 윗면 이음매
		}
		else
		{
			const auto AddGround = [&](const FVector3& Center, const FVector2& Size) {
				const FEntity Ground             = Scene.CreateEntity("Ground");
				Scene.GetTransform(Ground).Position = Center;
				FBoxCollider2DComponent& Box     = Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Ground);
				Box.Size                         = Size;
				Box.Friction                     = 0.0f;
			};
			AddGround(FVector3(250.0f, 0.0f, 50.0f), FVector2(500.0f, 100.0f));
			AddGround(FVector3(750.0f, 0.0f, 75.0f), FVector2(500.0f, 50.0f));
		}
		const FEntity Mover = Scene.CreateEntity("Mover");
		if (bCircle)
		{
			Scene.GetTransform(Mover).Position = FVector3(100.0f, 0.0f, 125.0f);
			FCircleCollider2DComponent& Circle = Scene.GetRegistry().Emplace<FCircleCollider2DComponent>(Mover);
			Circle.Radius                      = 25.0f;
			Circle.Friction                    = 0.0f;
			Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Mover);
		}
		else
		{
			Scene.GetTransform(Mover).Position = FVector3(100.0f, 0.0f, 125.0f);
			FBoxCollider2DComponent& Box       = Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Mover);
			Box.Size                           = FVector2(50.0f, 50.0f);
			Box.Friction                       = 0.0f;
			Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Mover).bFixedRotation = true;
		}
		Scene.UpdateTransforms();

		FPhysics2DSystem Physics;
		Physics.Begin();
		Simulate(Physics, Scene, 0.5f); // 내려앉기
		Physics.SetVelocity(Mover, FVector2(600.0f, 0.0f));
		FResult Result;
		for (int32 Index = 0; Index < 75; ++Index) // 1.25초 ≈ 750cm (X 100 → 850, 이음매 X 500 통과)
		{
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
			Result.MinVelocityX   = std::min(Result.MinVelocityX, Physics.GetVelocity(Mover).X);
			Result.MaxHeightError = std::max(Result.MaxHeightError, std::abs(Scene.GetTransform(Mover).Position.Z - 125.0f));
		}
		Result.EndX = Scene.GetTransform(Mover).Position.X;
		Physics.End();
		return Result;
	};
	for (const bool bCircle : { false, true })
	{
		const FResult Tiles = Run(true, bCircle);
		const FResult Boxes = Run(false, bCircle);
		E_LOG(LogPhysics, Display, "[이음매] {}: 타일 체인 최소 vx {:.1f} 높이 오차 {:.2f} 끝 X {:.0f} / 상자 둘 최소 vx {:.1f} 높이 오차 {:.2f} 끝 X {:.0f}",
		      bCircle ? "원" : "상자", Tiles.MinVelocityX, Tiles.MaxHeightError, Tiles.EndX, Boxes.MinVelocityX, Boxes.MaxHeightError, Boxes.EndX);
		E_EXPECT_TRUE(Tiles.MinVelocityX > 600.0f * 0.98f);
		E_EXPECT_TRUE(Tiles.MaxHeightError < 1.5f);
		E_EXPECT_TRUE(Tiles.EndX > 800.0f);
	}
}
