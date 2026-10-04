// 타일맵 충돌 → 2D 물리 (FPhysics2DSystem::BuildTilemapShapes — Physics/Physics2DSystem.h 머리 주석): 타일 위 정지, 원웨이 타일,
// 타일을 지우면 바디를 다시 만들어 떨어짐, TileData 문자열이 바뀌어도(복제·Undo) 다시 만듦. 타일셋은 임시 폴더에 파일로 쓴다
#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement2D.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DSystem.h"
#include "Physics/PhysicsWorld.h" // LogPhysics
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"
#include "Scene/Sprite/TilemapCollision.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	// 타일 0 = Full, 1 = Full + OneWay, 2 = 오른쪽으로 오르는 45도 경사 다각형 (16px, 셀 크기는 컴포넌트가 100cm로 정한다)
	std::string WriteTileset()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectETilemap2DPhysicsTests";
		std::filesystem::create_directories(Directory);
		FTilesetAsset Tileset;
		Tileset.Texture       = "Tiles.png";
		Tileset.TextureWidth  = 48;
		Tileset.TextureHeight = 16;
		FTileDefinition Ground;
		Ground.Id        = 0;
		Ground.Collision = ETileCollision::Full;
		FTileDefinition Platform;
		Platform.Id        = 1;
		Platform.Collision = ETileCollision::Full;
		Platform.bOneWay   = true;
		FTileDefinition Slope;
		Slope.Id        = 2;
		Slope.Collision = ETileCollision::Polygon;
		Slope.Points    = { FVector2(0.0f, 16.0f), FVector2(16.0f, 16.0f), FVector2(16.0f, 0.0f) }; // 왼쪽 아래 → 오른쪽 아래 → 오른쪽 위
		Tileset.Tiles   = { Ground, Platform, Slope };
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

// ---------------------------------------------------------------- 경사 다각형 타일 ↔ Full 외곽선 (TilemapCollision::TraceSolidOutlines)

namespace
{
	using FCellLoop = std::vector<FVector2>;

	FCellLoop Square(float X, float Y) { return { FVector2(X, Y), FVector2(X + 1.0f, Y), FVector2(X + 1.0f, Y + 1.0f), FVector2(X, Y + 1.0f) }; }
} // namespace

// Full 칸만이면 칸 격자 외곽선(TraceOutlines)과 같다. 경사 삼각형·반 칸 경사는 이웃 Full 칸과 한 고리로 합쳐지고(맞닿은 변·부분 겹침 지움),
// 외딴 삼각형은 첫 변 가운데 점을 더해 4점 (Box2D 닫힌 체인 최소)
E_TEST(TilemapCollision_SolidOutlineMergesSlopesWithFull)
{
	// Full만: 구멍 있는 고리 + 대각 칸 — 격자 결과와 같다
	const std::vector<FTileCoord> Cells = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 0, 1 }, { 2, 1 }, { 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 3 } };
	std::vector<FCellLoop>        Squares;
	for (const FTileCoord& Cell : Cells)
	{
		Squares.push_back(Square(static_cast<float>(Cell.X), static_cast<float>(Cell.Y)));
	}
	const std::vector<std::vector<FTileCoord>> Grid  = TilemapCollision::TraceOutlines(Cells);
	const std::vector<FCellLoop>               Solid = TilemapCollision::TraceSolidOutlines(Squares);
	E_EXPECT_EQ(Solid.size(), Grid.size());
	for (size_t Loop = 0; Loop < std::min(Solid.size(), Grid.size()); ++Loop)
	{
		E_EXPECT_EQ(Solid[Loop].size(), Grid[Loop].size());
		for (size_t Index = 0; Index < std::min(Solid[Loop].size(), Grid[Loop].size()); ++Index)
		{
			E_EXPECT_TRUE(Solid[Loop][Index] == FVector2(static_cast<float>(Grid[Loop][Index].X), static_cast<float>(Grid[Loop][Index].Y)));
		}
	}

	// 오르막 삼각형 (0,0)-(1,0)-(1,1) + 오른쪽 Full (1,0) + 아래 줄 Full (0,-1)·(1,-1): 경사 빗변이 고원 윗면과 이어진 고리 하나 (시계 방향 입력도)
	const std::vector<FCellLoop> Ramp = TilemapCollision::TraceSolidOutlines(
		{ { FVector2(1.0f, 1.0f), FVector2(1.0f, 0.0f), FVector2(0.0f, 0.0f) }, Square(1.0f, 0.0f), Square(0.0f, -1.0f), Square(1.0f, -1.0f) });
	E_EXPECT_EQ(Ramp.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(!Ramp.empty() && Ramp[0] == (FCellLoop{ FVector2(0.0f, -1.0f), FVector2(2.0f, -1.0f), FVector2(2.0f, 1.0f), FVector2(1.0f, 1.0f), FVector2(0.0f, 0.0f) }));

	// 반 칸 경사 (0,0)-(1,0)-(1,0.5) + 오른쪽 Full: Full 왼변의 아래 절반만 지워진다 (위 절반은 벽으로 남음)
	const std::vector<FCellLoop> Half =
		TilemapCollision::TraceSolidOutlines({ { FVector2(0.0f, 0.0f), FVector2(1.0f, 0.0f), FVector2(1.0f, 0.5f) }, Square(1.0f, 0.0f) });
	E_EXPECT_EQ(Half.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(!Half.empty() && Half[0] == (FCellLoop{ FVector2(0.0f, 0.0f), FVector2(2.0f, 0.0f), FVector2(2.0f, 1.0f), FVector2(1.0f, 1.0f), FVector2(1.0f, 0.5f) }));

	// 외딴 삼각형: 3점 → 첫 변 가운데 점을 더해 4점
	const std::vector<FCellLoop> Lonely = TilemapCollision::TraceSolidOutlines({ { FVector2(5.0f, 5.0f), FVector2(6.0f, 5.0f), FVector2(6.0f, 6.0f) } });
	E_EXPECT_EQ(Lonely.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(!Lonely.empty() && Lonely[0] == (FCellLoop{ FVector2(5.0f, 5.0f), FVector2(5.5f, 5.0f), FVector2(6.0f, 5.0f), FVector2(6.0f, 6.0f) }));

	// BuildShapes: 다각형 타일이 외곽선에 합쳐진다 (Polygons 목록은 동적 강체 타일맵용으로 그대로)
	FTilesetAsset Tileset;
	Tileset.TextureWidth  = 48;
	Tileset.TextureHeight = 16;
	FTileDefinition Full;
	Full.Id        = 0;
	Full.Collision = ETileCollision::Full;
	FTileDefinition Slope;
	Slope.Id        = 2;
	Slope.Collision = ETileCollision::Polygon;
	Slope.Points    = { FVector2(0.0f, 16.0f), FVector2(16.0f, 16.0f), FVector2(16.0f, 0.0f) };
	Tileset.Tiles   = { Full, Slope };
	FTilemapData Data;
	Data.Set(0, 0, TileCell::Make(2));
	Data.Set(1, 0, TileCell::Make(0));
	const FTilemapCollisionShapes Shapes = TilemapCollision::BuildShapes(Data, Tileset, FVector2(100.0f, 50.0f));
	E_EXPECT_EQ(Shapes.Outlines.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(!Shapes.Outlines.empty() && !Shapes.Outlines[0].bHole &&
	              Shapes.Outlines[0].Points == (std::vector<FVector2>{ FVector2(0.0f, 0.0f), FVector2(200.0f, 0.0f), FVector2(200.0f, 50.0f), FVector2(100.0f, 50.0f) }));
	E_EXPECT_EQ(Shapes.Polygons.size(), static_cast<size_t>(1));
}

namespace
{
	// 경사 타일맵: 아래 줄 Full(y 0, X -10~40칸), 오르막 경사(x 8, y 1), 고원 Full(y 1, x 9~40칸). 셀 50cm → 바닥 윗면 Z 50, 고원 윗면 Z 100.
	// bSeparatePolygons면 경사를 타일 대신 같은 모양의 다각형 콜라이더 엔티티로 (이전 방식 비교 — 경사와 고원 체인이 따로)
	FEntity BuildRampTilemap(FScene& Scene, const std::string& TilesetPath, bool bSeparatePolygons)
	{
		const FEntity      Map     = AddTilemap(Scene, TilesetPath);
		FTilemapComponent& Tilemap = Scene.GetRegistry().Get<FTilemapComponent>(Map);
		Tilemap.CellSize           = FVector2(50.0f, 50.0f);
		Tilemap.Friction           = 0.0f;
		FillRow(Tilemap, -10, 40, 0, 0);
		FillRow(Tilemap, 9, 40, 1, 0);
		if (bSeparatePolygons)
		{
			const FEntity Ramp = Scene.CreateEntity("Ramp");
			FPolygonCollider2DComponent& Polygon = Scene.GetRegistry().Emplace<FPolygonCollider2DComponent>(Ramp);
			Polygon.Points   = "400,50; 450,50; 450,100";
			Polygon.Friction = 0.0f;
		}
		else
		{
			FillRow(Tilemap, 8, 8, 1, 2);
		}
		return Map;
	}
} // namespace

// 평지 → 경사 → 고원 이음매: 마찰 없이 미끄러지는 원/상자가 경사 다각형과 Full 체인이 만나는 꼭짓점(경사 아래·위)에 걸리지 않고
// 고원 위를 계속 간다 (경사가 외곽선 체인에 합쳐져 고스트 꼭짓점이 없다). 비교로 경사를 따로 둔 이전 방식도 잰다(로그만 — 상자는 경사
// 아래 꼭짓점에 막혀 멈췄다)
E_TEST(Tilemap2DPhysics_SlopeToFlatSeamKeepsVelocity)
{
	const std::string TilesetPath = WriteTileset();
	struct FResult
	{
		float PlateauVelocityX = 0.0f; // 고원 위(X 500~)를 지날 때 최소 vx (경사 끝에서 살짝 떠올랐다 내려앉는 것은 정상)
		float EndZ             = 0.0f;
		float EndX             = 0.0f;
	};
	const auto Run = [&](bool bSeparatePolygons, bool bCircle) {
		FScene Scene;
		BuildRampTilemap(Scene, TilesetPath, bSeparatePolygons);
		const FEntity Mover                = Scene.CreateEntity("Mover");
		Scene.GetTransform(Mover).Position = FVector3(100.0f, 0.0f, 75.0f);
		if (bCircle)
		{
			FCircleCollider2DComponent& Circle = Scene.GetRegistry().Emplace<FCircleCollider2DComponent>(Mover);
			Circle.Radius                      = 25.0f;
			Circle.Friction                    = 0.0f;
			Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Mover).bFixedRotation = true;
		}
		else
		{
			FBoxCollider2DComponent& Box = Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Mover);
			Box.Size                     = FVector2(50.0f, 50.0f);
			Box.Friction                 = 0.0f;
			Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Mover).bFixedRotation = true;
		}
		Scene.UpdateTransforms();
		FPhysics2DSystem Physics;
		Physics.Begin();
		Simulate(Physics, Scene, 0.3f);
		Physics.SetVelocity(Mover, FVector2(900.0f, 0.0f));
		FResult Result;
		Result.PlateauVelocityX = 1.0e9f;
		for (int32 Index = 0; Index < 120; ++Index)
		{
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
			const FVector3 Position = Scene.GetTransform(Mover).Position;
			if (Position.X > 500.0f)
			{
				Result.PlateauVelocityX = std::min(Result.PlateauVelocityX, Physics.GetVelocity(Mover).X);
			}
		}
		Result.EndX = Scene.GetTransform(Mover).Position.X;
		Result.EndZ = Scene.GetTransform(Mover).Position.Z;
		Physics.End();
		return Result;
	};
	for (const bool bCircle : { false, true })
	{
		const FResult Merged   = Run(false, bCircle);
		const FResult Separate = Run(true, bCircle);
		E_LOG(LogPhysics, Display, "[경사 이음매] {}: 합친 체인 고원 최소 vx {:.1f} 끝 ({:.0f}, {:.1f}) / 따로 둔 경사 고원 최소 vx {:.1f} 끝 ({:.0f}, {:.1f})",
		      bCircle ? "원" : "상자", Merged.PlateauVelocityX, Merged.EndX, Merged.EndZ, Separate.PlateauVelocityX, Separate.EndX, Separate.EndZ);
		// 900 → 경사 진입(×cos45 ≈ 636) → 중력 감속(50cm 오름) ≈ 550 → 고원 진입(×cos45) ≈ 390cm/s
		E_EXPECT_TRUE(Merged.PlateauVelocityX > 300.0f && Merged.PlateauVelocityX < 1.0e8f);
		E_EXPECT_NEAR(Merged.EndZ, 125.0f, 2.0f); // 고원 위에 내려앉아 미끄러지는 중
		E_EXPECT_TRUE(Merged.EndX > 900.0f);
	}
}

// 2D 캐릭터 이동기: 타일맵 경사를 걸어 올라 고원으로, 다시 내려와 바닥으로 — 경사와 Full이 만나는 꼭짓점에 걸리지 않는다
E_TEST(Tilemap2DPhysics_CharacterRunsUpAndDownTileSlope)
{
	const std::string TilesetPath = WriteTileset();
	FScene            Scene;
	BuildRampTilemap(Scene, TilesetPath, false);
	const FEntity Pawn                = Scene.CreateEntity("Pawn");
	Scene.GetTransform(Pawn).Position = FVector3(0.0f, 0.0f, 120.0f);
	Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Pawn);
	Scene.UpdateTransforms();
	FPhysics2DSystem           Physics;
	FCharacterMovement2DSystem Characters;
	Physics.Begin();
	Characters.Begin(Physics);
	Physics.SyncBodies(Scene);
	const auto Run = [&](int32 Frames, float InputX) {
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Characters.Sync(Scene);
			FCharacterMove2D Move;
			Move.DeltaSeconds = Frame;
			Move.Input        = FVector2(InputX, 0.0f);
			Characters.SimulateCharacter(Scene, Pawn, Move);
			Characters.UpdateProxies();
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
		}
	};
	Run(30, 0.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Pawn).Position.Z, 50.0f + 60.0f, 2.0f); // 바닥 윗면 50 + 캡슐 반 높이 60
	Run(90, 1.0f); // 600cm/s × 1.5초 → X 0 → 경사(400~450) → 고원 위
	E_EXPECT_TRUE(Scene.GetTransform(Pawn).Position.X > 700.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Pawn).Position.Z, 100.0f + 60.0f, 2.0f);
	E_EXPECT_TRUE(Characters.IsGrounded(Pawn));
	Run(90, -1.0f); // 다시 내려와 바닥으로
	E_EXPECT_TRUE(Scene.GetTransform(Pawn).Position.X < 200.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Pawn).Position.Z, 50.0f + 60.0f, 2.0f);
	Characters.End();
	Physics.End();
}
