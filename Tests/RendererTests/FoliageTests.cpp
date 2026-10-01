#include "Core/Testing/TestFramework.h"
#include "Renderer/FoliageMeshes.h"
#include "Scene/Foliage.h"

#include <cmath>
#include <random>

// 풀·나무 배치 순수 로직 (Scene/Foliage.h) + 내장 절차 메시 (Renderer/FoliageMeshes.h)
namespace
{
	// 평평한 바닥 z = 100 (|x|, |y| < 5000 안)
	bool FlatSurface(float X, float Y, FVector3& OutPosition, FVector3& OutNormal)
	{
		if (std::abs(X) > 5000.0f || std::abs(Y) > 5000.0f)
		{
			return false;
		}
		OutPosition = FVector3(X, Y, 100.0f);
		OutNormal   = FVector3::UpVector;
		return true;
	}
} // namespace

E_TEST(Foliage_FadeAndSurfaceRules)
{
	E_EXPECT_NEAR(FoliageMath::ComputeFade(0.0f, 1000.0f), 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(FoliageMath::ComputeFade(850.0f, 1000.0f), 1.0f, 1.0e-6f); // 끝 15% 전
	E_EXPECT_NEAR(FoliageMath::ComputeFade(925.0f, 1000.0f), 0.5f, 1.0e-4f);
	E_EXPECT_NEAR(FoliageMath::ComputeFade(1000.0f, 1000.0f), 0.0f, 1.0e-6f);

	FFoliageType Type;
	Type.MaxSlope  = 30.0f;
	Type.MinHeight = 0.0f;
	Type.MaxHeight = 500.0f;
	E_EXPECT_TRUE(FoliageMath::AcceptsSurface(Type, FVector3(0, 0, 100), FVector3::UpVector));
	E_EXPECT_FALSE(FoliageMath::AcceptsSurface(Type, FVector3(0, 0, 600), FVector3::UpVector)); // 너무 높음
	const float Slope40 = FMath::DegreesToRadians(40.0f);
	E_EXPECT_FALSE(FoliageMath::AcceptsSurface(Type, FVector3(0, 0, 100), FVector3(std::sin(Slope40), 0.0f, std::cos(Slope40))));
	Type.MinHeight = Type.MaxHeight = 0.0f; // 같으면 높이 제한 없음
	E_EXPECT_TRUE(FoliageMath::AcceptsSurface(Type, FVector3(0, 0, 1.0e6f), FVector3::UpVector));
}

E_TEST(Foliage_WorldMatrixAlignsToNormal)
{
	FFoliageType Type;
	Type.bAlignToNormal = true;
	Type.ZOffset        = -10.0f;
	FFoliageInstance Instance;
	Instance.Position  = FVector3(100.0f, 200.0f, 50.0f);
	Instance.Scale     = 2.0f;
	Instance.Yaw       = 30.0f;
	Instance.Normal    = FVector3(1.0f, 0.0f, 1.0f).GetNormalized();
	const FMatrix4x4 World = FoliageMath::MakeWorldMatrix(Instance, Type, 1.0f);
	// 로컬 +Z(위)가 법선 방향, 크기 2
	const FVector3 Up = World.TransformVector(FVector3::UpVector);
	E_EXPECT_NEAR(Up.Length(), 2.0f, 1.0e-4f);
	E_EXPECT_TRUE(Up.GetNormalized().Equals(Instance.Normal, 1.0e-4f));
	// 원점 = 위치 + 법선 방향으로 ZOffset * 크기
	E_EXPECT_TRUE(World.GetOrigin().Equals(Instance.Position + Instance.Normal * (-20.0f), 1.0e-3f));
	// 페이드 배율은 크기에 곱해진다
	E_EXPECT_NEAR(FoliageMath::MakeWorldMatrix(Instance, Type, 0.5f).TransformVector(FVector3::UpVector).Length(), 1.0f, 1.0e-4f);
	Type.bAlignToNormal = false;
	E_EXPECT_TRUE(FoliageMath::MakeWorldMatrix(Instance, Type, 1.0f).TransformVector(FVector3::UpVector).GetNormalized().Equals(FVector3::UpVector, 1.0e-5f));
}

E_TEST(Foliage_PaintFillsToDensityAndErase)
{
	FFoliageAsset Asset;
	FFoliageType& Type = Asset.Types.emplace_back();
	Type.Density       = 20.0f; // 100m²당
	Asset.EnsureInstanceLists();
	std::mt19937 Random(7);
	// 반경 10m = 314m² → 목표 62개
	const uint32 Added = FoliageMath::Paint(Asset, 0, FVector2(0.0f, 0.0f), 1000.0f, 1.0f, 10000, FlatSurface, Random);
	E_EXPECT_EQ(Added, 62u);
	E_EXPECT_EQ(FoliageMath::CountInCircle(Asset.Instances[0], FVector2(0.0f, 0.0f), 1000.0f), 62u);
	for (const FFoliageInstance& Instance : Asset.Instances[0])
	{
		E_EXPECT_NEAR(Instance.Position.Z, 100.0f, 1.0e-4f);
		E_EXPECT_TRUE(Instance.Scale >= Type.MinScale && Instance.Scale <= Type.MaxScale);
	}
	// 이미 목표 밀도면 더 심지 않는다, 한 번 최대 개수 제한
	E_EXPECT_EQ(FoliageMath::Paint(Asset, 0, FVector2(0.0f, 0.0f), 1000.0f, 1.0f, 10000, FlatSurface, Random), 0u);
	E_EXPECT_EQ(FoliageMath::Paint(Asset, 0, FVector2(3000.0f, 0.0f), 1000.0f, 1.0f, 5, FlatSurface, Random), 5u);
	// 지면이 없는 곳(밖)에는 심지 않는다
	E_EXPECT_EQ(FoliageMath::Paint(Asset, 0, FVector2(9000.0f, 9000.0f), 500.0f, 1.0f, 100, FlatSurface, Random), 0u);

	const uint64 Counter = Asset.ChangeCounter;
	const uint32 Removed = FoliageMath::Erase(Asset, 0, FVector2(0.0f, 0.0f), 500.0f);
	E_EXPECT_TRUE(Removed > 0);
	E_EXPECT_EQ(FoliageMath::CountInCircle(Asset.Instances[0], FVector2(0.0f, 0.0f), 500.0f), 0u);
	E_EXPECT_TRUE(Asset.ChangeCounter > Counter);
	E_EXPECT_EQ(Asset.TypeCounters[0], Asset.ChangeCounter);
}

E_TEST(Foliage_JsonRoundTrip)
{
	FFoliageAsset Asset;
	Asset.Types.resize(2);
	Asset.Types[1].Name       = "Tree";
	Asset.Types[1].Mesh       = "foliage:tree";
	Asset.Types[1].bCollision = true;
	Asset.EnsureInstanceLists();
	FFoliageInstance Instance;
	Instance.Position = FVector3(1.0f, 2.0f, 3.0f);
	Instance.Yaw      = 45.0f;
	Asset.Instances[1].push_back(Instance);
	Asset.Revision = 9;
	FFoliageAsset Loaded;
	std::string   Error;
	E_EXPECT_TRUE(FoliageIO::FromJsonString(FoliageIO::ToJsonString(Asset), Loaded, &Error));
	E_EXPECT_EQ(Loaded.Types.size(), size_t(2));
	E_EXPECT_EQ(Loaded.Types[1].Mesh, std::string("foliage:tree"));
	E_EXPECT_TRUE(Loaded.Types[1].bCollision);
	E_EXPECT_EQ(Loaded.Instances[0].size(), size_t(0));
	E_EXPECT_EQ(Loaded.Instances[1].size(), size_t(1));
	E_EXPECT_TRUE(Loaded.Instances[1][0].Position.Equals(Instance.Position, 0.0f));
	E_EXPECT_NEAR(Loaded.Instances[1][0].Yaw, 45.0f, 0.0f);
	E_EXPECT_EQ(Loaded.Revision, 9u);
	E_EXPECT_FALSE(FoliageIO::FromJsonString("{\"Types\": 3}", Loaded, &Error));
}

E_TEST(FoliageMeshes_BuildAllWithCwWinding)
{
	for (const char* Name : { "grass", "bush", "tree", "pine", "rock" })
	{
		FMeshData Mesh;
		E_EXPECT_TRUE(FoliageMeshes::Build(Name, Mesh));
		E_EXPECT_TRUE(!Mesh.Indices.empty() && Mesh.Indices.size() % 3 == 0);
		// 와인딩 규약: Cross(P1-P0, P2-P0)·정점 법선 평균 > 0 (CW 앞면)
		uint32 Wrong = 0;
		for (size_t Index = 0; Index < Mesh.Indices.size(); Index += 3)
		{
			const FVertex& A = Mesh.Vertices[Mesh.Indices[Index]];
			const FVertex& B = Mesh.Vertices[Mesh.Indices[Index + 1]];
			const FVertex& C = Mesh.Vertices[Mesh.Indices[Index + 2]];
			const FVector3 Face = FVector3::Cross(B.Position - A.Position, C.Position - A.Position);
			Wrong += FVector3::Dot(Face, A.Normal + B.Normal + C.Normal) < 0.0f ? 1u : 0u;
		}
		E_EXPECT_TRUE(Wrong * 50 < Mesh.Indices.size() / 3); // 울퉁불퉁한 덩어리의 일부 접힌 면만 허용 (2% 미만)
	}
	FMeshData Unknown;
	E_EXPECT_FALSE(FoliageMeshes::Build("palm", Unknown));
	FMeshData Tree;
	FoliageMeshes::Build("tree", Tree);
	E_EXPECT_TRUE(!Tree.Lods.empty()); // 닫힌 큰 메시는 LOD
}
