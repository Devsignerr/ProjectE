#include "Core/Testing/TestFramework.h"
#include "Editor/NavMeshBaker.h"

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
