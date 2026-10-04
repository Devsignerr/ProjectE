#include "Core/Testing/TestFramework.h"
#include "Editor/Editor2D/Collider2DShapes.h"
#include "Editor/Editor2D/Editor2DMath.h"
#include "Editor/Editor2D/Editor2DScene.h"
#include "Editor/EditorCameraState.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DMath.h"
#include "Renderer/Camera.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/SpriteAsset.h"
#include "Scene/Sprite/TilemapData.h"

using namespace Editor2DMath;

namespace
{
	constexpr float Tol = 1.0e-3f;

	uint32 FlagsFromIndex(uint32 Index)
	{
		return ((Index & 1) ? TileCell::Rotate90Bit : 0u) | ((Index & 2) ? TileCell::FlipXBit : 0u) | ((Index & 4) ? TileCell::FlipYBit : 0u);
	}

	FTileStamp MakeStamp(int32 Width, int32 Height, std::vector<uint32> Cells)
	{
		FTileStamp Stamp;
		Stamp.Width  = Width;
		Stamp.Height = Height;
		Stamp.Cells  = std::move(Cells);
		return Stamp;
	}
} // namespace

// ---------------------------------------------------------------- 타일 변환 합성

E_TEST(Editor2D_ComposeFlagsMatchesTransformTileUv)
{
	const FVector2 Samples[] = { FVector2(0.3f, 0.1f), FVector2(-0.4f, 0.25f) };
	for (uint32 Outer = 0; Outer < 8; ++Outer)
	{
		for (uint32 Inner = 0; Inner < 8; ++Inner)
		{
			const uint32 Composed = ComposeTileFlags(FlagsFromIndex(Outer), FlagsFromIndex(Inner));
			for (const FVector2& Sample : Samples)
			{
				const FVector2 Expected = TilemapMath::TransformTileUv(TilemapMath::TransformTileUv(Sample, FlagsFromIndex(Inner)), FlagsFromIndex(Outer));
				const FVector2 Actual   = TilemapMath::TransformTileUv(Sample, Composed);
				E_EXPECT_NEAR(Actual.X, Expected.X, Tol);
				E_EXPECT_NEAR(Actual.Y, Expected.Y, Tol);
			}
		}
	}
	// 빈칸은 변환해도 빈칸, Id는 유지
	E_EXPECT_EQ(TransformCell(0u, TileCell::FlipXBit), 0u);
	E_EXPECT_EQ(TransformCell(TileCell::Make(7, TileCell::FlipXBit), TileCell::FlipXBit), TileCell::Make(7));
}

// ---------------------------------------------------------------- 스탬프

E_TEST(Editor2D_TransformStampRotateFlipAndIdentity)
{
	const uint32     A     = TileCell::Make(0);
	const uint32     B     = TileCell::Make(1);
	const FTileStamp Wide  = MakeStamp(2, 1, { A, B }); // 왼쪽 A, 오른쪽 B

	// 반시계 90도: 오른쪽 → 위 (세로 1x2, 아래 A 위 B), 칸마다 Rotate90
	const FTileStamp Rotated = TransformStamp(Wide, TileCell::Rotate90Bit);
	E_EXPECT_EQ(Rotated.Width, 1);
	E_EXPECT_EQ(Rotated.Height, 2);
	E_EXPECT_EQ(Rotated.Get(0, 0), TileCell::Make(0, TileCell::Rotate90Bit));
	E_EXPECT_EQ(Rotated.Get(0, 1), TileCell::Make(1, TileCell::Rotate90Bit));

	// 좌우 반전: 순서가 바뀌고 칸마다 FlipX
	const FTileStamp Flipped = TransformStamp(Wide, TileCell::FlipXBit);
	E_EXPECT_EQ(Flipped.Width, 2);
	E_EXPECT_EQ(Flipped.Get(0, 0), TileCell::Make(1, TileCell::FlipXBit));
	E_EXPECT_EQ(Flipped.Get(1, 0), TileCell::Make(0, TileCell::FlipXBit));

	// 90도 네 번 = 원래, 같은 반전 두 번 = 원래
	const FTileStamp Square = MakeStamp(2, 3, { A, B, TileCell::Make(2), 0u, TileCell::Make(4), TileCell::Make(5, TileCell::FlipYBit) });
	FTileStamp       Turned = Square;
	for (int32 Step = 0; Step < 4; ++Step)
	{
		Turned = TransformStamp(Turned, TileCell::Rotate90Bit);
	}
	E_EXPECT_TRUE(Turned == Square);
	E_EXPECT_TRUE(TransformStamp(TransformStamp(Square, TileCell::FlipYBit), TileCell::FlipYBit) == Square);
}

E_TEST(Editor2D_MakeStampFromTilesetBottomRowFirst)
{
	// 4열 타일셋에서 (1,0) ~ (2,1) 사각형: 스탬프 0행 = 이미지 아래 행(1)
	const FTileStamp Stamp = MakeStampFromTileset(4, 2, 1, 1, 0);
	E_EXPECT_EQ(Stamp.Width, 2);
	E_EXPECT_EQ(Stamp.Height, 2);
	E_EXPECT_EQ(Stamp.Get(0, 0), TileCell::Make(5));
	E_EXPECT_EQ(Stamp.Get(1, 0), TileCell::Make(6));
	E_EXPECT_EQ(Stamp.Get(0, 1), TileCell::Make(1));
	E_EXPECT_EQ(Stamp.Get(1, 1), TileCell::Make(2));
	// 열 범위 밖은 잘린다
	E_EXPECT_EQ(MakeStampFromTileset(4, 3, 0, 9, 0).Width, 1);
}

E_TEST(Editor2D_PaintStampOriginSkipsEmptyAndErases)
{
	FTilemapData Data;
	Data.Set(5, 5, TileCell::Make(9));
	// 3x3 스탬프, 가운데 칸 빈칸 → 커서 (5, 5) = 스탬프 칸 (1, 1) → 왼쪽 아래 (4, 4)
	const uint32     T     = TileCell::Make(3);
	const FTileStamp Stamp = MakeStamp(3, 3, { T, T, T, T, 0u, T, T, T, T });
	E_EXPECT_EQ(GetStampOrigin(Stamp, FTileCoord{ 5, 5 }).X, 4);
	E_EXPECT_EQ(PaintStamp(Data, Stamp, FTileCoord{ 5, 5 }, false), 8);
	E_EXPECT_EQ(Data.Get(4, 4), T);
	E_EXPECT_EQ(Data.Get(6, 6), T);
	E_EXPECT_EQ(Data.Get(5, 5), TileCell::Make(9)); // 빈칸은 건드리지 않음
	E_EXPECT_EQ(PaintStamp(Data, Stamp, FTileCoord{ 5, 5 }, false), 0); // 같은 값이면 바뀐 칸 없음
	// 지우개 = 스탬프 모양만큼
	E_EXPECT_EQ(PaintStamp(Data, Stamp, FTileCoord{ 5, 5 }, true), 8);
	E_EXPECT_EQ(Data.GetCellCount(), static_cast<size_t>(1));
	// 짝수 크기: 커서 = 칸 ((W-1)/2, (H-1)/2) = (0, 0)
	E_EXPECT_EQ(GetStampOrigin(MakeStamp(2, 2, { T, T, T, T }), FTileCoord{ 0, 0 }).Y, 0);
}

E_TEST(Editor2D_FillRectPatternRepeatsFromRectCorner)
{
	FTilemapData     Data;
	const uint32     A       = TileCell::Make(1);
	const uint32     B       = TileCell::Make(2);
	const FTileStamp Pattern = MakeStamp(2, 1, { A, B });
	E_EXPECT_EQ(FillRectPattern(Data, FTileRect::FromCorners(-1, 0, 2, 1), Pattern, false), 8);
	E_EXPECT_EQ(Data.Get(-1, 0), A);
	E_EXPECT_EQ(Data.Get(0, 0), B);
	E_EXPECT_EQ(Data.Get(1, 1), A);
	E_EXPECT_EQ(Data.Get(2, 1), B);
	E_EXPECT_EQ(SamplePattern(Pattern, -2, 7, FTileCoord{ -1, 0 }), B); // 음수 방향도 같은 주기
	// 지우개 사각형은 전부
	E_EXPECT_EQ(FillRectPattern(Data, FTileRect::FromCorners(0, 0, 2, 1), Pattern, true), 6);
	E_EXPECT_EQ(Data.GetCellCount(), static_cast<size_t>(2));
}

E_TEST(Editor2D_LineStampMatchesDrawLineForSingleTile)
{
	FTilemapData Expected;
	FTilemapData Actual;
	const uint32 Cell = TileCell::Make(4, TileCell::FlipYBit);
	Expected.DrawLine(-3, 2, 7, -4, Cell);
	E_EXPECT_EQ(LineStamp(Actual, FTileCoord{ -3, 2 }, FTileCoord{ 7, -4 }, FTileStamp::Single(Cell), false), static_cast<int32>(Expected.GetCellCount()));
	E_EXPECT_TRUE(Actual == Expected);
}

E_TEST(Editor2D_FloodFillPatternBoundedAndTooLarge)
{
	// 벽(타일 1)으로 둘러싼 3x2 빈칸 방을 타일 2로 채운다 — 바깥은 그대로
	FTilemapData Data;
	const uint32 Wall = TileCell::Make(1);
	Data.FillRect(FTileRect::FromCorners(0, 0, 4, 3), Wall);
	Data.FillRect(FTileRect::FromCorners(1, 1, 3, 2), 0u);
	const FTileStamp Fill = FTileStamp::Single(TileCell::Make(2));
	E_EXPECT_EQ(FloodFillPattern(Data, FTileCoord{ 2, 1 }, Fill, false, ComputeFloodLimit(Data, FTileCoord{ 2, 1 })), 6);
	E_EXPECT_EQ(Data.Get(1, 1), TileCell::Make(2));
	E_EXPECT_EQ(Data.Get(0, 0), Wall);
	E_EXPECT_EQ(Data.Get(-1, 0), 0u);

	// 바깥 빈칸: 경계 상한 = (칠한 영역 ∪ 시작 칸) + 2칸 = X -3~6, Y -3~5 → 10x9 - 20(채운 칸) = 70칸
	FTilemapData Open = Data;
	E_EXPECT_EQ(FloodFillPattern(Open, FTileCoord{ -1, -1 }, Fill, false, ComputeFloodLimit(Open, FTileCoord{ -1, -1 }, 2)), 10 * 9 - 20);
	// 최대 칸 수를 넘으면 아무것도 바꾸지 않고 -1
	FTilemapData Small = Data;
	E_EXPECT_EQ(FloodFillPattern(Small, FTileCoord{ -1, -1 }, Fill, false, ComputeFloodLimit(Small, FTileCoord{ -1, -1 }, 2), 10), -1);
	E_EXPECT_TRUE(Small == Data);
	// 흐름 지우기
	E_EXPECT_EQ(FloodFillPattern(Data, FTileCoord{ 0, 0 }, Fill, true, ComputeFloodLimit(Data, FTileCoord{ 0, 0 })), 14);
	E_EXPECT_EQ(Data.GetCellCount(), static_cast<size_t>(6));
}

// ---------------------------------------------------------------- 2D 뷰

E_TEST(Editor2D_ScreenToPlaneMatchesCameraRay)
{
	const FVector2 ImageSize(1280.0f, 720.0f);
	FCamera        Camera;
	Camera.SetPerspective(60.0f, ImageSize.X / ImageSize.Y, 10.0f, 100000.0f);
	Camera.SetOrthographic(800.0f, ImageSize.X / ImageSize.Y, 10.0f, 100000.0f);
	Camera.SetRotation(Get2DCameraRotation());
	Camera.SetPosition(FVector3(120.0f, 5000.0f, -40.0f));
	// 앞 = -Y, 오른쪽 = +X, 위 = +Z
	E_EXPECT_NEAR(Camera.GetForwardVector().Y, -1.0f, Tol);
	E_EXPECT_NEAR(Camera.GetRightVector().X, 1.0f, Tol);
	E_EXPECT_NEAR(Camera.GetUpVector().Z, 1.0f, Tol);

	const FVector2 Pixels[] = { FVector2(0.0f, 0.0f), FVector2(640.0f, 360.0f), FVector2(1000.0f, 100.0f), FVector2(13.0f, 700.0f) };
	for (const FVector2& Pixel : Pixels)
	{
		const FRay Ray = Editor2DScene::MakeRay(Camera.GetViewProjectionMatrix(), Pixel, ImageSize);
		FVector3   Hit;
		E_EXPECT_TRUE(Editor2DScene::RayToPlaneY(Ray, 0.0f, Hit));
		const FVector2 Plane = ScreenToPlane(FVector2(120.0f, -40.0f), 800.0f, ImageSize, Pixel);
		E_EXPECT_NEAR(Plane.X, Hit.X, 0.05f);
		E_EXPECT_NEAR(Plane.Y, Hit.Z, 0.05f);
		const FVector2 Back = PlaneToScreen(FVector2(120.0f, -40.0f), 800.0f, ImageSize, Plane);
		E_EXPECT_NEAR(Back.X, Pixel.X, Tol);
		E_EXPECT_NEAR(Back.Y, Pixel.Y, Tol);
	}

	// 커서 기준 줌: 커서 아래 평면 점은 그대로
	const FVector2 Cursor(1000.0f, 100.0f);
	const FVector2 Anchor = ScreenToPlane(FVector2(120.0f, -40.0f), 800.0f, ImageSize, Cursor);
	const FVector2 Moved  = ZoomAroundCursor(FVector2(120.0f, -40.0f), 800.0f, 400.0f, ImageSize, Cursor);
	const FVector2 After  = ScreenToPlane(Moved, 400.0f, ImageSize, Cursor);
	E_EXPECT_NEAR(After.X, Anchor.X, Tol);
	E_EXPECT_NEAR(After.Y, Anchor.Y, Tol);
}

E_TEST(Editor2D_Rotation2DCounterClockwiseAndSnap)
{
	// 2D 각 +90도: 로컬 +X가 화면 위(+Z)로 — 물리 규약과 같은 회전
	const FVector3 Rotated = MakeRotation2D(90.0f).RotateVector(FVector3::ForwardVector);
	E_EXPECT_NEAR(Rotated.X, 0.0f, Tol);
	E_EXPECT_NEAR(Rotated.Z, 1.0f, Tol);
	E_EXPECT_TRUE(MakeRotation2D(30.0f).Equals(Physics2DMath::RotationFromAngle(FMath::DegreesToRadians(30.0f)), 1.0e-4f));

	E_EXPECT_NEAR(SnapToStep(74.0f, 50.0f), 50.0f, Tol);
	E_EXPECT_NEAR(SnapToStep(76.0f, 50.0f), 100.0f, Tol);
	E_EXPECT_NEAR(SnapToStep(-26.0f, 50.0f), -50.0f, Tol);
	E_EXPECT_NEAR(SnapToStep(13.0f, 10.0f, 5.0f), 15.0f, Tol);
	E_EXPECT_NEAR(SnapToStep(13.3f, 0.0f), 13.3f, Tol);
}

// ---------------------------------------------------------------- 2D 클릭 판정

E_TEST(Editor2D_SpriteRectHitUsesPivotSizeAndFlip)
{
	FSpriteSlice Slice;
	Slice.W     = 20;
	Slice.H     = 10;
	Slice.Pivot = FVector2(0.0f, 0.0f); // 왼쪽 아래
	// 1px = 2cm → 40 x 20, 피벗 왼쪽 아래: X [0, 40], Z [0, 20]
	const FSpriteQuad Quad = SpriteMath::ComputeQuad(Slice, 64, 64, 2.0f, FVector2::ZeroVector, false, false);
	E_EXPECT_TRUE(IsPointInConvexPolygon(Quad.Positions, 4, FVector2(39.0f, 19.0f)));
	E_EXPECT_FALSE(IsPointInConvexPolygon(Quad.Positions, 4, FVector2(-1.0f, 5.0f)));
	// 좌우 반전 = 피벗을 지나는 세로축 거울: X [-40, 0]
	const FSpriteQuad Flipped = SpriteMath::ComputeQuad(Slice, 64, 64, 2.0f, FVector2::ZeroVector, true, false);
	E_EXPECT_TRUE(IsPointInConvexPolygon(Flipped.Positions, 4, FVector2(-39.0f, 5.0f)));
	E_EXPECT_FALSE(IsPointInConvexPolygon(Flipped.Positions, 4, FVector2(1.0f, 5.0f)));
}

E_TEST(Editor2D_FindFrontmostOrdersLayerOrderDepth)
{
	std::vector<FPick2DHit> Hits;
	E_EXPECT_EQ(FindFrontmost(Hits), -1);
	Hits.push_back({ FEntity{ 1, 0 }, 1, 0, 5, 100.0f });
	Hits.push_back({ FEntity{ 2, 0 }, 1, 1, 0, 300.0f }); // 레이어가 높다 → 순번·거리 무관하게 위
	Hits.push_back({ FEntity{ 3, 0 }, 0, 9, 9, 1.0f });   // 콜라이더만 = 뒤 순위
	E_EXPECT_EQ(FindFrontmost(Hits), 1);
	Hits[1].SortLayer = 0; // 같은 레이어 → 순번 큰 것
	E_EXPECT_EQ(FindFrontmost(Hits), 0);
	Hits[1].Order = 5; // 같은 순번 → 가까운 것
	E_EXPECT_EQ(FindFrontmost(Hits), 0);
	Hits[1].Distance = 50.0f;
	E_EXPECT_EQ(FindFrontmost(Hits), 1);
	Hits[1].Distance = 100.0f; // 완전히 같으면 먼저 넣은 것
	E_EXPECT_EQ(FindFrontmost(Hits), 0);
}

E_TEST(Editor2D_PickTilemapCellsAndCollidersInScene)
{
	FScene Scene;
	// 콜라이더만 있는 엔티티: 회전 90도 + 스케일 X 2인 100x20 상자 → 바디 공간 200x20 → 세로로 선 20(X) x 200(Z)
	const FEntity Wall = Scene.CreateEntity("Wall");
	Scene.GetTransform(Wall).Position = FVector3(500.0f, 0.0f, 0.0f);
	Scene.GetTransform(Wall).Rotation = MakeRotation2D(90.0f);
	Scene.GetTransform(Wall).Scale    = FVector3(2.0f, 1.0f, 1.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Wall).Size = FVector2(100.0f, 20.0f);
	// 타일맵 (타일셋 없음 → 컴포넌트 셀 크기): 칸 (1, 0)만 칠함
	const FEntity      Map     = Scene.CreateEntity("Map");
	FTilemapComponent& Tilemap = Scene.GetRegistry().Emplace<FTilemapComponent>(Map);
	Tilemap.CellSize           = FVector2(50.0f, 50.0f);
	FTilemapData Data;
	Data.Set(1, 0, TileCell::Make(0));
	Tilemap.TileData = Data.Encode();
	Scene.UpdateTransforms();

	auto PickAt = [&](float X, float Z) {
		const FRay Ray(FVector3(X, 5000.0f, Z), FVector3(0.0f, -1.0f, 0.0f));
		float      Distance = 0.0f;
		return Editor2DScene::Pick(Scene, Ray, 2.0f, Distance);
	};
	E_EXPECT_TRUE(PickAt(75.0f, 25.0f) == Map);   // 칠한 칸
	E_EXPECT_FALSE(PickAt(25.0f, 25.0f).IsValid()); // 빈 칸
	E_EXPECT_TRUE(PickAt(505.0f, 90.0f) == Wall); // 세운 상자 위쪽
	E_EXPECT_FALSE(PickAt(530.0f, 0.0f).IsValid()); // 폭 20 밖

	// 외곽선도 같은 모양 (네 꼭짓점, 깊이 = 엔티티 Y)
	std::vector<Collider2DShapes::FOutline> Outlines;
	Collider2DShapes::Collect(Scene, Wall, Outlines);
	E_EXPECT_EQ(Outlines.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Outlines[0].Points.size(), static_cast<size_t>(4));
	FBox Bounds;
	E_EXPECT_TRUE(Editor2DScene::AddBounds(Scene, Wall, Bounds));
	E_EXPECT_NEAR(Bounds.Max.Z - Bounds.Min.Z, 200.0f, 0.1f);
	E_EXPECT_NEAR(Bounds.Max.X - Bounds.Min.X, 20.0f, 0.1f);
}

E_TEST(Editor2D_CameraStateKeepsViewport2DAndSaved3D)
{
	FEditorCameraState State;
	State.bViewport2D          = true;
	State.bHasSaved3D          = true;
	State.Saved3DPosition      = FVector3(1.0f, 2.0f, 3.0f);
	State.Saved3DRotation      = FQuat::FromEuler(-20.0f, 45.0f, 0.0f);
	State.bSaved3DOrthographic = true;
	State.Saved3DOrthoHeight   = 777.0f;
	FEditorCameraState Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(State.ToJsonString()));
	E_EXPECT_TRUE(Loaded.bViewport2D);
	E_EXPECT_TRUE(Loaded.bHasSaved3D);
	E_EXPECT_NEAR(Loaded.Saved3DPosition.Z, 3.0f, Tol);
	E_EXPECT_TRUE(Loaded.Saved3DRotation.Equals(State.Saved3DRotation, 1.0e-4f));
	E_EXPECT_TRUE(Loaded.bSaved3DOrthographic);
	E_EXPECT_NEAR(Loaded.Saved3DOrthoHeight, 777.0f, Tol);
	// 예전 파일(2D 키 없음)은 3D
	FEditorCameraState Old;
	E_EXPECT_TRUE(Old.FromJsonString(R"({"Position":[0,0,0],"Rotation":[0,0,0,1]})"));
	E_EXPECT_FALSE(Old.bViewport2D);
	E_EXPECT_FALSE(Old.bHasSaved3D);
}

// ---- 박스 선택 (Editor2DMath "박스 선택")

E_TEST(Editor2D_BoxSelectProjectsBoundsAndRequiresContainment)
{
	// 2D 카메라: +Y에서 -Y, 직교 높이 1000cm, 이미지 1000x1000 → 1px = 1cm, 화면 가운데 = 원점, 오른쪽 = +X, 위 = +Z
	FCamera Camera;
	Camera.SetPosition(FVector3(0.0f, 5000.0f, 0.0f));
	Camera.SetRotation(Get2DCameraRotation());
	Camera.SetOrthographic(1000.0f, 1.0f, 10.0f, 20000.0f);
	const FVector2 ImageSize(1000.0f, 1000.0f);

	FScreenRect Rect;
	E_EXPECT_TRUE(ProjectBoundsToScreen(FBox(FVector3(100.0f, -5.0f, 50.0f), FVector3(200.0f, 5.0f, 150.0f)), Camera.GetViewProjectionMatrix(), ImageSize, Rect));
	E_EXPECT_NEAR(Rect.Min.X, 600.0f, 0.5f);
	E_EXPECT_NEAR(Rect.Max.X, 700.0f, 0.5f);
	E_EXPECT_NEAR(Rect.Min.Y, 350.0f, 0.5f); // 위(+Z 150) = 화면 위쪽(작은 y)
	E_EXPECT_NEAR(Rect.Max.Y, 450.0f, 0.5f);

	// 완전히 들어가야 선택 (걸치기는 아님)
	E_EXPECT_TRUE(IsScreenRectInside(Rect, MakeScreenRect(FVector2(710.0f, 460.0f), FVector2(590.0f, 340.0f))));
	E_EXPECT_FALSE(IsScreenRectInside(Rect, MakeScreenRect(FVector2(650.0f, 340.0f), FVector2(710.0f, 460.0f))));

	// 무효 경계·카메라 뒤 (원근) = 실패
	E_EXPECT_FALSE(ProjectBoundsToScreen(FBox(), Camera.GetViewProjectionMatrix(), ImageSize, Rect));
	FCamera Perspective;
	Perspective.SetPerspective(60.0f, 1.0f, 10.0f, 100000.0f);
	Perspective.SetPosition(FVector3::ZeroVector); // 앞 = +X
	E_EXPECT_FALSE(ProjectBoundsToScreen(FBox(FVector3(-200.0f, -10.0f, -10.0f), FVector3(-100.0f, 10.0f, 10.0f)), Perspective.GetViewProjectionMatrix(), ImageSize, Rect));
	E_EXPECT_TRUE(ProjectBoundsToScreen(FBox(FVector3(100.0f, -10.0f, -10.0f), FVector3(200.0f, 10.0f, 10.0f)), Perspective.GetViewProjectionMatrix(), ImageSize, Rect));
	E_EXPECT_TRUE(Rect.Min.X < 500.0f && Rect.Max.X > 500.0f);

	// 합치기
	FScreenRect Union;
	bool        bValid = false;
	UnionScreenRect(Union, bValid, FScreenRect{ FVector2(10.0f, 20.0f), FVector2(30.0f, 40.0f) });
	UnionScreenRect(Union, bValid, FScreenRect{ FVector2(5.0f, 25.0f), FVector2(20.0f, 50.0f) });
	E_EXPECT_TRUE(bValid);
	E_EXPECT_NEAR(Union.Min.X, 5.0f, Tol);
	E_EXPECT_NEAR(Union.Min.Y, 20.0f, Tol);
	E_EXPECT_NEAR(Union.Max.X, 30.0f, Tol);
	E_EXPECT_NEAR(Union.Max.Y, 50.0f, Tol);
}

E_TEST(Editor2D_BoxSelectCombineModes)
{
	const FEntity A{ 1, 1 };
	const FEntity B{ 2, 1 };
	const FEntity C{ 3, 1 };
	const std::vector<FEntity> Current = { A, B };
	const std::vector<FEntity> Hits    = { B, C, C };

	const std::vector<FEntity> Replace = CombineBoxSelection(Current, Hits, EBoxSelectMode::Replace);
	E_EXPECT_EQ(Replace.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Replace[0] == B && Replace[1] == C);
	E_EXPECT_TRUE(CombineBoxSelection(Current, {}, EBoxSelectMode::Replace).empty()); // 빈 곳 끌기 = 선택 해제

	const std::vector<FEntity> Add = CombineBoxSelection(Current, Hits, EBoxSelectMode::Add);
	E_EXPECT_EQ(Add.size(), static_cast<size_t>(3));
	E_EXPECT_TRUE(Add[0] == A && Add[1] == B && Add[2] == C);

	const std::vector<FEntity> Toggle = CombineBoxSelection(Current, Hits, EBoxSelectMode::Toggle);
	E_EXPECT_EQ(Toggle.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Toggle[0] == A && Toggle[1] == C); // B는 빠지고 C는 더해짐
}

// 9-슬라이스 스프라이트의 클릭/박스 판정 사각형 = 늘인 크기 그대로 (렌더러가 그리는 바깥 크기 SpriteMath::ComputeSize와 같음)
E_TEST(Editor2D_NineSliceQuadMatchesDrawnSize)
{
	FSpriteSlice Slice;
	Slice.Name         = "Panel";
	Slice.W            = 16;
	Slice.H            = 16;
	Slice.BorderLeft   = 4;
	Slice.BorderRight  = 4;
	Slice.BorderTop    = 4;
	Slice.BorderBottom = 4;
	const FVector2    Size(300.0f, 120.0f);
	const FSpriteQuad Quad  = SpriteMath::ComputeQuad(Slice, 64, 64, 2.0f, Size, false, false);
	const FVector2    Drawn = SpriteMath::ComputeSize(Slice, 2.0f, Size);
	E_EXPECT_NEAR(Drawn.X, 300.0f, Tol);
	E_EXPECT_NEAR(Quad.Positions[2].X - Quad.Positions[0].X, Drawn.X, Tol);
	E_EXPECT_NEAR(Quad.Positions[2].Y - Quad.Positions[0].Y, Drawn.Y, Tol);
	// Size 0 = 슬라이스 원래 크기 × UnitsPerPixel
	const FSpriteQuad Native = SpriteMath::ComputeQuad(Slice, 64, 64, 2.0f, FVector2::ZeroVector, false, false);
	E_EXPECT_NEAR(Native.Positions[2].X - Native.Positions[0].X, 32.0f, Tol);
}
