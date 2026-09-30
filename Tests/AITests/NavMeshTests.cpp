#include "AI/Navigation/NavCoordinates.h"
#include "AI/Navigation/NavMesh.h"
#include "Core/Testing/TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace
{
	// 사각형 하나(삼각형 2개)를 추가한다. 네 점은 둘레 순서이고, Normal 쪽이 앞면이 되도록 와인딩을 맞춘다
	// (엔진 규약: Cross(P1 - P0, P2 - P0)가 앞면 노멀)
	void AddQuad(FNavMeshBuildInput& Input, const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& D, const FVector3& Normal)
	{
		const uint32 Base = static_cast<uint32>(Input.Vertices.size());
		Input.Vertices.push_back(A);
		Input.Vertices.push_back(B);
		Input.Vertices.push_back(C);
		Input.Vertices.push_back(D);
		const bool bFacing = FVector3::Dot(FVector3::Cross(B - A, C - A), Normal) > 0.0f;
		if (bFacing)
		{
			Input.Indices.insert(Input.Indices.end(), { Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
		}
		else
		{
			Input.Indices.insert(Input.Indices.end(), { Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
		}
	}

	// Z 높이의 수평 바닥. bFacingUp = false이면 아래(-Z)를 향한다
	void AddFloor(FNavMeshBuildInput& Input, float MinX, float MinY, float MaxX, float MaxY, float Z, bool bFacingUp = true)
	{
		AddQuad(Input, FVector3(MinX, MinY, Z), FVector3(MaxX, MinY, Z), FVector3(MaxX, MaxY, Z), FVector3(MinX, MaxY, Z),
		        bFacingUp ? FVector3::UpVector : -FVector3::UpVector);
	}

	// 바깥을 향한 박스 (윗면 + 옆면 4개, 바닥면은 바닥에 붙으므로 생략)
	void AddBox(FNavMeshBuildInput& Input, const FVector3& Min, const FVector3& Max)
	{
		const FVector3 P000(Min.X, Min.Y, Min.Z), P100(Max.X, Min.Y, Min.Z), P110(Max.X, Max.Y, Min.Z), P010(Min.X, Max.Y, Min.Z);
		const FVector3 P001(Min.X, Min.Y, Max.Z), P101(Max.X, Min.Y, Max.Z), P111(Max.X, Max.Y, Max.Z), P011(Min.X, Max.Y, Max.Z);
		AddQuad(Input, P001, P101, P111, P011, FVector3(0.0f, 0.0f, 1.0f));
		AddQuad(Input, P000, P010, P011, P001, FVector3(-1.0f, 0.0f, 0.0f));
		AddQuad(Input, P100, P110, P111, P101, FVector3(1.0f, 0.0f, 0.0f));
		AddQuad(Input, P000, P100, P101, P001, FVector3(0.0f, -1.0f, 0.0f));
		AddQuad(Input, P010, P110, P111, P011, FVector3(0.0f, 1.0f, 0.0f));
	}

	// 바닥 2000×2000cm + 가운데 벽 (X -50~50, Y -600~600, 높이 300cm)
	constexpr float WallHalfX = 50.0f;
	constexpr float WallHalfY = 600.0f;

	FNavMeshBuildInput MakeFloorWithWall()
	{
		FNavMeshBuildInput Input;
		AddFloor(Input, -1000.0f, -1000.0f, 1000.0f, 1000.0f, 0.0f);
		AddBox(Input, FVector3(-WallHalfX, -WallHalfY, 0.0f), FVector3(WallHalfX, WallHalfY, 300.0f));
		return Input;
	}

	float PathLength(const std::vector<FVector3>& Points)
	{
		float Length = 0.0f;
		for (size_t Index = 1; Index < Points.size(); ++Index)
		{
			Length += FVector3::Distance(Points[Index - 1], Points[Index]);
		}
		return Length;
	}

	// 경로 선분이 벽의 XY 영역 안쪽을 지나는지 (5cm 간격 표본)
	bool PathCrossesWall(const std::vector<FVector3>& Points)
	{
		for (size_t Index = 1; Index < Points.size(); ++Index)
		{
			const FVector3 A        = Points[Index - 1];
			const FVector3 B        = Points[Index];
			const int32    Samples  = std::max(1, static_cast<int32>(FVector3::Distance(A, B) / 5.0f));
			for (int32 Sample = 0; Sample <= Samples; ++Sample)
			{
				const FVector3 P = FVector3::Lerp(A, B, static_cast<float>(Sample) / static_cast<float>(Samples));
				if (std::abs(P.X) < WallHalfX && std::abs(P.Y) < WallHalfY)
				{
					return true;
				}
			}
		}
		return false;
	}

	FNavMesh BuildOrFail(const FNavMeshBuildInput& Input)
	{
		FNavMesh    NavMesh;
		std::string Error;
		const bool  bBuilt = NavMesh.Build(Input, FNavMeshBuildSettings(), &Error);
		if (!bBuilt)
		{
			FTestRegistry::ReportFailure(__FILE__, __LINE__, "내비메시 굽기 실패: " + Error);
		}
		return NavMesh;
	}
}

// ---------------------------------------------------------------- 좌표 변환

E_TEST(NavCoordinates_PointRoundTrip)
{
	const FVector3 Points[] = { FVector3(0.0f, 0.0f, 0.0f), FVector3(100.0f, 200.0f, 300.0f), FVector3(-1234.5f, 678.25f, -90.0f) };
	for (const FVector3& Point : Points)
	{
		float Recast[3];
		FNavCoordinates::ToRecast(Point, Recast);
		E_EXPECT_EQUALS(FNavCoordinates::FromRecast(Recast), Point, 1e-3f);
	}

	// recast (x, y, z) = (X, Z, Y) × 0.01 — 엔진 위(+Z)는 Recast 위(+y)
	float Recast[3];
	FNavCoordinates::ToRecast(FVector3(100.0f, 200.0f, 300.0f), Recast);
	E_EXPECT_NEAR(Recast[0], 1.0f, 1e-6f);
	E_EXPECT_NEAR(Recast[1], 3.0f, 1e-6f);
	E_EXPECT_NEAR(Recast[2], 2.0f, 1e-6f);
}

E_TEST(NavCoordinates_UnitScale)
{
	E_EXPECT_NEAR(FNavCoordinates::ToMeters(100.0f), 1.0f, 1e-6f);
	E_EXPECT_NEAR(FNavCoordinates::ToUnits(2.5f), 250.0f, 1e-4f);
	E_EXPECT_NEAR(FNavCoordinates::ToUnits(FNavCoordinates::ToMeters(37.0f)), 37.0f, 1e-4f);
}

E_TEST(NavCoordinates_WindingFlipKeepsFloorFacingUp)
{
	// 엔진에서 +Z를 향한 삼각형
	const FVector3 P[3] = { FVector3(0.0f, 0.0f, 0.0f), FVector3(100.0f, 0.0f, 0.0f), FVector3(0.0f, 100.0f, 0.0f) };
	E_EXPECT_TRUE(FVector3::Cross(P[1] - P[0], P[2] - P[0]).Z > 0.0f);

	float R[3][3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FNavCoordinates::ToRecast(P[Index], R[Index]);
	}
	// Recast 노멀 = Cross(v1 - v0, v2 - v0). 인덱스를 그대로 두면 아래(-y), 뒤집으면 위(+y)
	const auto RecastNormalY = [&](int32 I0, int32 I1, int32 I2)
	{
		const FVector3 V0(R[I0][0], R[I0][1], R[I0][2]);
		const FVector3 V1(R[I1][0], R[I1][1], R[I1][2]);
		const FVector3 V2(R[I2][0], R[I2][1], R[I2][2]);
		return FVector3::Cross(V1 - V0, V2 - V0).Y;
	};
	E_EXPECT_TRUE(RecastNormalY(0, 1, 2) < 0.0f);

	int32 I0 = 0, I1 = 1, I2 = 2;
	FNavCoordinates::FlipWinding(I0, I1, I2);
	E_EXPECT_TRUE(RecastNormalY(I0, I1, I2) > 0.0f);
}

// ---------------------------------------------------------------- 굽기 + 경로

E_TEST(NavMesh_UpFacingFloorIsWalkable)
{
	FNavMeshBuildInput Input;
	AddFloor(Input, -500.0f, -500.0f, 500.0f, 500.0f, 0.0f);
	const FNavMesh NavMesh = BuildOrFail(Input);
	E_EXPECT_TRUE(NavMesh.IsValid());
	E_EXPECT_TRUE(NavMesh.GetPolygonCount() > 0);

	std::vector<FVector3> Points;
	const FVector3        End(300.0f, 250.0f, 0.0f);
	E_EXPECT_TRUE(NavMesh.FindPath(FVector3(-300.0f, -300.0f, 0.0f), End, Points) == ENavPathResult::Complete);
	E_EXPECT_TRUE(Points.size() >= 2);
	if (!Points.empty())
	{
		E_EXPECT_NEAR(Points.back().X, End.X, 1.0f);
		E_EXPECT_NEAR(Points.back().Y, End.Y, 1.0f);
		E_EXPECT_NEAR(Points.back().Z, End.Z, 15.0f);
	}
}

E_TEST(NavMesh_DownFacingFloorIsNotWalkable)
{
	FNavMeshBuildInput Input;
	AddFloor(Input, -500.0f, -500.0f, 500.0f, 500.0f, 0.0f, false);
	FNavMesh    NavMesh;
	std::string Error;
	E_EXPECT_FALSE(NavMesh.Build(Input, FNavMeshBuildSettings(), &Error));
	E_EXPECT_FALSE(NavMesh.IsValid());
	E_EXPECT_FALSE(Error.empty());
}

E_TEST(NavMesh_PathGoesAroundWall)
{
	const FNavMesh NavMesh = BuildOrFail(MakeFloorWithWall());

	const FVector3        Start(-500.0f, 0.0f, 0.0f);
	const FVector3        End(500.0f, 0.0f, 0.0f);
	std::vector<FVector3> Points;
	E_EXPECT_TRUE(NavMesh.FindPath(Start, End, Points) == ENavPathResult::Complete);
	E_EXPECT_TRUE(Points.size() >= 3); // 벽을 돌아가려면 꺾이는 점이 있어야 한다

	// 직선 거리(1000cm)보다 길고, 벽 영역을 지나지 않는다
	E_EXPECT_TRUE(PathLength(Points) > FVector3::Distance(Start, End) + 100.0f);
	E_EXPECT_FALSE(PathCrossesWall(Points));
	if (!Points.empty())
	{
		E_EXPECT_NEAR(Points.front().X, Start.X, 1.0f);
		E_EXPECT_NEAR(Points.back().X, End.X, 1.0f);
		E_EXPECT_NEAR(Points.back().Y, End.Y, 1.0f);
	}
}

E_TEST(NavMesh_DisconnectedPlatformsHaveNoCompletePath)
{
	FNavMeshBuildInput Input;
	AddFloor(Input, -1000.0f, -500.0f, -200.0f, 500.0f, 0.0f);
	AddFloor(Input, 200.0f, -500.0f, 1000.0f, 500.0f, 0.0f);
	const FNavMesh NavMesh = BuildOrFail(Input);

	std::vector<FVector3> Points;
	const ENavPathResult  Result = NavMesh.FindPath(FVector3(-600.0f, 0.0f, 0.0f), FVector3(600.0f, 0.0f, 0.0f), Points);
	E_EXPECT_TRUE(Result == ENavPathResult::Partial || Result == ENavPathResult::Failed);
	if (Result == ENavPathResult::Partial)
	{
		// 가장 가까이 간 곳은 왼쪽 발판 위
		E_EXPECT_TRUE(!Points.empty() && Points.back().X < -150.0f);
	}
}

E_TEST(NavMesh_ProjectPointDropsToFloor)
{
	FNavMeshBuildInput Input;
	AddFloor(Input, -500.0f, -500.0f, 500.0f, 500.0f, 0.0f);
	const FNavMesh NavMesh = BuildOrFail(Input);

	FVector3 Projected;
	E_EXPECT_TRUE(NavMesh.ProjectPoint(FVector3(100.0f, -120.0f, 120.0f), Projected));
	E_EXPECT_NEAR(Projected.X, 100.0f, 1.0f);
	E_EXPECT_NEAR(Projected.Y, -120.0f, 1.0f);
	E_EXPECT_NEAR(Projected.Z, 0.0f, 15.0f);

	// 검색 범위 밖이면 실패
	E_EXPECT_FALSE(NavMesh.ProjectPoint(FVector3(5000.0f, 5000.0f, 0.0f), Projected));
}

E_TEST(NavMesh_DebugTrianglesFaceUp)
{
	const FNavMesh        NavMesh = BuildOrFail(MakeFloorWithWall());
	std::vector<FVector3> Triangles;
	NavMesh.GetDebugTriangles(Triangles);
	E_EXPECT_TRUE(!Triangles.empty());
	E_EXPECT_EQ(Triangles.size() % 3, size_t(0));

	int32 DownFacing = 0;
	for (size_t Index = 0; Index + 2 < Triangles.size(); Index += 3)
	{
		const FVector3 Normal = FVector3::Cross(Triangles[Index + 1] - Triangles[Index], Triangles[Index + 2] - Triangles[Index]);
		if (Normal.Length() > 1e-2f && Normal.Z <= 0.0f)
		{
			++DownFacing;
		}
	}
	E_EXPECT_EQ(DownFacing, 0);
}

// ---------------------------------------------------------------- 직렬화

E_TEST(NavMesh_SaveLoadKeepsPath)
{
	const FNavMesh NavMesh = BuildOrFail(MakeFloorWithWall());
	const FVector3 Start(-500.0f, 100.0f, 0.0f);
	const FVector3 End(600.0f, -200.0f, 0.0f);

	std::vector<FVector3> Expected;
	E_EXPECT_TRUE(NavMesh.FindPath(Start, End, Expected) == ENavPathResult::Complete);

	const std::vector<uint8> Bytes = NavMesh.SaveToBytes();
	E_EXPECT_TRUE(!Bytes.empty());

	FNavMesh    Loaded;
	std::string Error;
	E_EXPECT_TRUE(Loaded.LoadFromBytes(Bytes, &Error));
	E_EXPECT_TRUE(Loaded.IsValid());
	E_EXPECT_EQ(Loaded.GetPolygonCount(), NavMesh.GetPolygonCount());
	E_EXPECT_NEAR(Loaded.GetSettings().AgentRadius, NavMesh.GetSettings().AgentRadius, 0.0f);

	std::vector<FVector3> Actual;
	E_EXPECT_TRUE(Loaded.FindPath(Start, End, Actual) == ENavPathResult::Complete);
	E_EXPECT_EQ(Actual.size(), Expected.size());
	for (size_t Index = 0; Index < std::min(Actual.size(), Expected.size()); ++Index)
	{
		E_EXPECT_EQUALS(Actual[Index], Expected[Index], 1e-4f);
	}

	// 파일 왕복
	const std::filesystem::path Path = FTestRegistry::GetTempDirectory() / (std::string("NavMeshTest") + FNavMesh::FileExtension);
	E_EXPECT_TRUE(NavMesh.SaveToFile(Path));
	FNavMesh FromFile;
	E_EXPECT_TRUE(FromFile.LoadFromFile(Path, &Error));
	std::vector<FVector3> FilePath;
	E_EXPECT_TRUE(FromFile.FindPath(Start, End, FilePath) == ENavPathResult::Complete);
	E_EXPECT_EQ(FilePath.size(), Expected.size());

	// 이동
	FNavMesh Moved = std::move(FromFile);
	E_EXPECT_TRUE(Moved.IsValid());
	E_EXPECT_TRUE(Moved.FindPath(Start, End, FilePath) == ENavPathResult::Complete);
}

E_TEST(NavMesh_InvalidInputFails)
{
	FNavMesh    NavMesh;
	std::string Error;
	E_EXPECT_FALSE(NavMesh.Build(FNavMeshBuildInput(), FNavMeshBuildSettings(), &Error));
	E_EXPECT_FALSE(NavMesh.IsValid());

	FNavMeshBuildInput OutOfRange;
	AddFloor(OutOfRange, -500.0f, -500.0f, 500.0f, 500.0f, 0.0f);
	OutOfRange.Indices.back() = 99;
	E_EXPECT_FALSE(NavMesh.Build(OutOfRange, FNavMeshBuildSettings(), &Error));

	FNavMeshBuildInput NotTriangles;
	AddFloor(NotTriangles, -500.0f, -500.0f, 500.0f, 500.0f, 0.0f);
	NotTriangles.Indices.pop_back();
	E_EXPECT_FALSE(NavMesh.Build(NotTriangles, FNavMeshBuildSettings(), &Error));

	FNavMeshBuildInput    Valid;
	FNavMeshBuildSettings BadSettings;
	AddFloor(Valid, -500.0f, -500.0f, 500.0f, 500.0f, 0.0f);
	BadSettings.CellSize = 0.0f;
	E_EXPECT_FALSE(NavMesh.Build(Valid, BadSettings, &Error));

	// 유효하지 않은 내비메시의 쿼리는 실패만 한다
	std::vector<FVector3> Points;
	FVector3              Projected;
	E_EXPECT_TRUE(NavMesh.FindPath(FVector3(), FVector3(100.0f, 0.0f, 0.0f), Points) == ENavPathResult::Failed);
	E_EXPECT_TRUE(Points.empty());
	E_EXPECT_FALSE(NavMesh.ProjectPoint(FVector3(), Projected));
	E_EXPECT_TRUE(NavMesh.SaveToBytes().empty());
}

E_TEST(NavMesh_InvalidDataLoadFails)
{
	FNavMesh    NavMesh;
	std::string Error;
	E_EXPECT_FALSE(NavMesh.LoadFromBytes(std::vector<uint8>(), &Error));
	E_EXPECT_FALSE(NavMesh.LoadFromBytes(std::vector<uint8>(64, 0xCD), &Error));
	E_EXPECT_FALSE(NavMesh.IsValid());

	FNavMeshBuildInput Input;
	AddFloor(Input, -500.0f, -500.0f, 500.0f, 500.0f, 0.0f);
	const FNavMesh           Source = BuildOrFail(Input);
	const std::vector<uint8> Bytes  = Source.SaveToBytes();
	E_EXPECT_TRUE(Bytes.size() > 64);

	// 잘린 데이터
	const std::vector<uint8> Truncated(Bytes.begin(), Bytes.begin() + static_cast<std::ptrdiff_t>(Bytes.size() / 2));
	E_EXPECT_FALSE(NavMesh.LoadFromBytes(Truncated, &Error));

	// 타일 데이터 한 바이트 손상 (해시로 검출)
	std::vector<uint8> Corrupted = Bytes;
	Corrupted[Corrupted.size() / 2] ^= 0xFF;
	E_EXPECT_FALSE(NavMesh.LoadFromBytes(Corrupted, &Error));

	// 버전 불일치
	std::vector<uint8> WrongVersion = Bytes;
	WrongVersion[4] ^= 0x7F;
	E_EXPECT_FALSE(NavMesh.LoadFromBytes(WrongVersion, &Error));
	E_EXPECT_FALSE(NavMesh.IsValid());

	// 없는 파일
	E_EXPECT_FALSE(NavMesh.LoadFromFile(FTestRegistry::GetTempDirectory() / "Missing.enav", &Error));

	// 로드 실패는 기존 내용을 지우지 않는다
	E_EXPECT_TRUE(NavMesh.LoadFromBytes(Bytes, &Error));
	E_EXPECT_FALSE(NavMesh.LoadFromBytes(Corrupted, &Error));
	E_EXPECT_TRUE(NavMesh.IsValid());
}
