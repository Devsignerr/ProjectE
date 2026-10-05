#include "Core/Testing/TestFramework.h"
#include "Editor/NavMeshBaker.h"
#include "Scene/Terrain.h"

namespace
{
	// 위(+Z)를 향한 바닥 사각형 (엔진 와인딩: Cross(P1 - P0, P2 - P0)가 앞면)
	void MakeFloor(std::vector<FVector3>& OutPositions, std::vector<uint32>& OutIndices)
	{
		OutPositions = { FVector3(-500.0f, -500.0f, 0.0f), FVector3(500.0f, -500.0f, 0.0f), FVector3(500.0f, 500.0f, 0.0f), FVector3(-500.0f, 500.0f, 0.0f) };
		OutIndices   = { 0, 1, 2, 0, 2, 3 };
	}

	bool AllFacingUp(const FNavMeshBuildInput& Input)
	{
		for (size_t Index = 0; Index + 2 < Input.Indices.size(); Index += 3)
		{
			const FVector3& P0 = Input.Vertices[Input.Indices[Index]];
			const FVector3& P1 = Input.Vertices[Input.Indices[Index + 1]];
			const FVector3& P2 = Input.Vertices[Input.Indices[Index + 2]];
			if (FVector3::Cross(P1 - P0, P2 - P0).Z <= 0.0f)
			{
				return false;
			}
		}
		return true;
	}
} // namespace

// 월드 변환 적용 + 인덱스 기준 이동, 거울상 스케일은 와인딩을 뒤집어 바닥이 계속 위를 향한다
E_TEST(NavMeshBaker_AppendMeshTransformsAndKeepsFacing)
{
	std::vector<FVector3> Positions;
	std::vector<uint32>   Indices;
	MakeFloor(Positions, Indices);
	E_EXPECT_TRUE(FVector3::Cross(Positions[1] - Positions[0], Positions[2] - Positions[0]).Z > 0.0f); // 입력 규약 확인

	FNavMeshBuildInput Input;
	FNavMeshBaker::AppendMesh(Positions, Indices, FMatrix4x4::MakeTranslation(FVector3(0.0f, 0.0f, 100.0f)), Input);
	E_EXPECT_EQ(Input.Vertices.size(), 4u);
	E_EXPECT_NEAR(Input.Vertices[0].Z, 100.0f, 1.0e-4f);
	E_EXPECT_TRUE(AllFacingUp(Input));

	// 두 번째 메시: X 거울상 → 인덱스는 기존 정점 뒤로 밀리고 방향은 유지
	FNavMeshBaker::AppendMesh(Positions, Indices, FMatrix4x4::MakeScale(FVector3(-1.0f, 1.0f, 1.0f)), Input);
	E_EXPECT_EQ(Input.Vertices.size(), 8u);
	E_EXPECT_EQ(Input.Indices.size(), 12u);
	E_EXPECT_TRUE(Input.Indices[6] >= 4u && Input.Indices[7] >= 4u && Input.Indices[8] >= 4u);
	E_EXPECT_TRUE(AllFacingUp(Input));
}

// 지형 높이장 → 격자 삼각형 (정점 = 해상도², 셀마다 둘, 위를 향함, 높이 = 프레임 변환과 같음)
E_TEST(NavMeshBaker_AppendTerrainGrid)
{
	FTerrainData Data;
	Data.Initialize(5);
	Data.Heights[2 * 5 + 3] = 40000; // 솟은 점 하나 (경사 면도 위를 향해야 한다)
	FTerrainComponent Component;
	Component.Size        = FVector2(400.0f, 400.0f);
	Component.HeightRange = 1000.0f;
	const FTerrainFrame Frame = FTerrainFrame::Make(FVector3(100.0f, 0.0f, 50.0f), Component, 5);

	FNavMeshBuildInput Input;
	Input.Vertices.push_back(FVector3()); // 기존 정점 뒤에 붙는지
	FNavMeshBaker::AppendTerrain(Data, Frame, Input);
	E_EXPECT_EQ(Input.Vertices.size(), 26u);
	E_EXPECT_EQ(Input.Indices.size(), 4u * 4u * 6u);
	E_EXPECT_TRUE(AllFacingUp(Input));
	const FVector3 Peak = Input.Vertices[1 + 2 * 5 + 3];
	E_EXPECT_NEAR(Peak.Z, Frame.HeightToWorldZ(40000.0f), 1.0e-3f);
	E_EXPECT_NEAR(Input.Vertices[1].Z, 50.0f, 0.02f); // 평평한 곳 ≈ 위치 Z (32768 = 범위 가운데에서 반 단계 위)
	E_EXPECT_NEAR(Input.Vertices[1].X, 100.0f - 200.0f, 1.0e-3f);
}

// 콜라이더 상자 → 닫힌 상자 12삼각형: 면마다 법선이 상자 가운데에서 바깥을 향하고, 윗면 둘은 위를 향한다 (걸을 수 있는 면)
E_TEST(NavMeshBaker_AppendBoxFacesOutward)
{
	FNavMeshBuildInput Input;
	const FMatrix4x4   World = FMatrix4x4::MakeScale(FVector3(2.0f, 1.0f, 1.0f)) * FMatrix4x4::MakeTranslation(FVector3(100.0f, 0.0f, 0.0f));
	FNavMeshBaker::AppendBox(FVector3(0.0f, 0.0f, 50.0f), FVector3(10.0f, 20.0f, 50.0f), World, Input);
	E_EXPECT_EQ(Input.Vertices.size(), 8u);
	E_EXPECT_EQ(Input.Indices.size(), 36u);
	const FVector3 Center(100.0f, 0.0f, 50.0f);
	uint32         UpFacing = 0;
	for (size_t Index = 0; Index + 2 < Input.Indices.size(); Index += 3)
	{
		const FVector3& P0     = Input.Vertices[Input.Indices[Index]];
		const FVector3& P1     = Input.Vertices[Input.Indices[Index + 1]];
		const FVector3& P2     = Input.Vertices[Input.Indices[Index + 2]];
		const FVector3  Normal = FVector3::Cross(P1 - P0, P2 - P0);
		const FVector3  Mid    = (P0 + P1 + P2) * (1.0f / 3.0f);
		E_EXPECT_TRUE(FVector3::Dot(Normal, Mid - Center) > 0.0f);
		UpFacing += Normal.Z > 0.0f && Mid.Z > 99.0f ? 1u : 0u;
	}
	E_EXPECT_EQ(UpFacing, 2u);
	E_EXPECT_NEAR(Input.Vertices[7].X, 120.0f, 1.0e-3f); // 반 크기 10 × 스케일 2
}
