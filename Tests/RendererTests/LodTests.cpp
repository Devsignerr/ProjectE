#include "Core/Math/Box.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/AssetCache.h"
#include "Renderer/LodMath.h"
#include "Renderer/MeshSimplifier.h"
#include "Renderer/ModelImportSettings.h"
#include "Renderer/PrimitiveShapes.h"

#include <cmath>
#include <map>
#include <tuple>

namespace
{
	// 삼각형 기하 법선이 꼭짓점 법선 평균과 같은 쪽인 비율 (CW 앞면 규약: Cross(P1-P0, P2-P0)·N > 0)
	float FrontFacingRatio(const std::vector<FVertex>& Vertices, const std::vector<uint32>& Indices)
	{
		uint32 Front = 0;
		for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
		{
			const FVertex& V0 = Vertices[Indices[Index]];
			const FVertex& V1 = Vertices[Indices[Index + 1]];
			const FVertex& V2 = Vertices[Indices[Index + 2]];
			const FVector3 Face = FVector3::Cross(V1.Position - V0.Position, V2.Position - V0.Position);
			if (FVector3::Dot(Face, V0.Normal + V1.Normal + V2.Normal) > 0.0f)
			{
				++Front;
			}
		}
		return Indices.empty() ? 0.0f : static_cast<float>(Front) / static_cast<float>(Indices.size() / 3);
	}
	// 위치 기준(같은 위치 = 같은 꼭짓점)으로 삼각형 하나에만 쓰이는 모서리 수. 닫힌 메시면 0
	uint32 CountOpenEdges(const std::vector<FVertex>& Vertices, const std::vector<uint32>& Indices)
	{
		std::map<std::tuple<long, long, long>, uint32> PositionIds; // 1/1000 단위로 반올림 (삼각 함수 오차)
		auto Id = [&](uint32 Vertex) {
			const FVector3& P = Vertices[Vertex].Position;
			const auto Key = std::make_tuple(std::lround(P.X * 1000.0f), std::lround(P.Y * 1000.0f), std::lround(P.Z * 1000.0f));
			return PositionIds.emplace(Key, static_cast<uint32>(PositionIds.size())).first->second;
		};
		std::map<std::pair<uint32, uint32>, int32> Edges;
		for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
		{
			const uint32 Ids[3] = { Id(Indices[Index]), Id(Indices[Index + 1]), Id(Indices[Index + 2]) };
			if (Ids[0] == Ids[1] || Ids[1] == Ids[2] || Ids[0] == Ids[2])
			{
				continue; // 퇴화 삼각형 (구의 극점)
			}
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 A = Ids[Corner];
				const uint32 B = Ids[(Corner + 1) % 3];
				++Edges[{ std::min(A, B), std::max(A, B) }];
			}
		}
		uint32 Open = 0;
		for (const auto& [Edge, Count] : Edges)
		{
			Open += Count == 1 ? 1u : 0u;
		}
		return Open;
	}
} // namespace

E_TEST(Lod_ScreenSizeAndSelection)
{
	// 반경 100, 거리 1000, tan(반 시야각) 0.5 → 100 / 500 = 0.2 (지름 / 화면 높이)
	E_EXPECT_NEAR(LodMath::ComputePerspectiveScreenSize(100.0f, 1000.0f, 0.5f), 0.2f, 1.0e-6f);
	E_EXPECT_TRUE(LodMath::ComputePerspectiveScreenSize(100.0f, 50.0f, 0.5f) > 1.0f); // 카메라가 구 안
	E_EXPECT_NEAR(LodMath::ComputeOrthographicScreenSize(50.0f, 1000.0f), 0.1f, 1.0e-6f);

	const float* Sizes = LodMath::DefaultScreenSizes; // 1, 0.5, 0.25, 0.12
	E_EXPECT_EQ(LodMath::SelectLod(2.0f, Sizes, 4), 0u);
	E_EXPECT_EQ(LodMath::SelectLod(0.6f, Sizes, 4), 0u);
	E_EXPECT_EQ(LodMath::SelectLod(0.4f, Sizes, 4), 1u);
	E_EXPECT_EQ(LodMath::SelectLod(0.2f, Sizes, 4), 2u);
	E_EXPECT_EQ(LodMath::SelectLod(0.01f, Sizes, 4), 3u);
	E_EXPECT_EQ(LodMath::SelectLod(0.01f, Sizes, 2), 1u); // LOD 수 제한
	E_EXPECT_EQ(LodMath::SelectLod(0.01f, Sizes, 1), 0u);
	E_EXPECT_EQ(LodMath::SelectLod(0.2f, Sizes, 4, 2.0f), 1u); // 배율 2 → 0.4로 보고 LOD1
	E_EXPECT_EQ(LodMath::SelectLod(0.5f, Sizes, 4), 0u);      // 임계값과 같으면 높은 품질 유지
}

E_TEST(Lod_HysteresisKeepsPreviousInsideBand)
{
	const float* Sizes = LodMath::DefaultScreenSizes; // 1, 0.5, 0.25, 0.12 — 여유 10%: LOD0↔1 띠 [0.45, 0.55)
	constexpr uint32 None = ~0u;
	// 처음 보는 인스턴스는 그냥 선택
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.48f, Sizes, 4, 1.0f, None, 0.1f), 1u);
	// 띠 안: 이전 LOD 유지 (내려가는 중 / 올라가는 중 모두)
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.48f, Sizes, 4, 1.0f, 0, 0.1f), 0u);
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.52f, Sizes, 4, 1.0f, 1, 0.1f), 1u);
	// 띠 밖: 바뀐다
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.44f, Sizes, 4, 1.0f, 0, 0.1f), 1u);
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.56f, Sizes, 4, 1.0f, 1, 0.1f), 0u);
	// 크게 바뀌면 여러 단계도 한 번에 (띠 밖으로 멀리)
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.01f, Sizes, 4, 1.0f, 0, 0.1f), 3u);
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(2.0f, Sizes, 4, 1.0f, 3, 0.1f), 0u);
	// 왕복: 0.5 근처에서 흔들려도 한 번 바뀐 뒤 띠 안에서는 그대로
	uint32 Lod = 0;
	for (const float Size : { 0.53f, 0.47f, 0.44f, 0.47f, 0.53f, 0.47f, 0.56f, 0.52f })
	{
		Lod = LodMath::SelectLodWithHysteresis(Size, Sizes, 4, 1.0f, Lod, 0.1f);
	}
	E_EXPECT_EQ(Lod, 0u); // 0.44에서 1로, 0.56에서 0으로 — 두 번만 바뀜
	// 여유 0 = 기존 선택, LOD 수 제한/배율도 같이 적용
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.48f, Sizes, 4, 1.0f, 0, 0.0f), 1u);
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.01f, Sizes, 2, 1.0f, 0, 0.1f), 1u);
	E_EXPECT_EQ(LodMath::SelectLodWithHysteresis(0.24f, Sizes, 4, 2.0f, 0, 0.1f), 0u); // 배율 2 → 0.48, 띠 안이라 0 유지
}

E_TEST(Lod_SimplifySphereReducesTrianglesAndKeepsWinding)
{
	FMeshData Sphere = FPrimitiveShapes::MakeSphere(50.0f, 32, 16);
	const uint32 SourceTriangles = static_cast<uint32>(Sphere.Indices.size() / 3);
	MeshSimplifier::GenerateLods(Sphere, LodMath::MaxLods);

	E_EXPECT_EQ(Sphere.Lods.size(), static_cast<size_t>(3));
	uint32 Previous = SourceTriangles;
	for (size_t Lod = 0; Lod < Sphere.Lods.size(); ++Lod)
	{
		const FMeshLod& Level     = Sphere.Lods[Lod];
		const uint32    Triangles = static_cast<uint32>(Level.Indices.size() / 3);
		E_EXPECT_EQ(Level.Indices.size() % 3, static_cast<size_t>(0));
		E_EXPECT_TRUE(Triangles > 0 && Triangles < Previous);
		E_EXPECT_TRUE(static_cast<float>(Triangles) <= LodMath::DefaultTriangleRatios[Lod + 1] * static_cast<float>(SourceTriangles) + 1.0f);
		E_EXPECT_NEAR(Level.ScreenSize, LodMath::DefaultScreenSizes[Lod + 1], 0.0f);
		for (const uint32 Index : Level.Indices)
		{
			E_EXPECT_TRUE(Index < Sphere.Vertices.size());
		}
		E_EXPECT_TRUE(FrontFacingRatio(Sphere.Vertices, Level.Indices) > 0.95f); // 와인딩 유지
		E_EXPECT_EQ(CountOpenEdges(Sphere.Vertices, Level.Indices), 0u);         // 닫힌 메시는 닫힌 채 (구멍 없음)
		Previous = Triangles;
	}
	E_EXPECT_TRUE(FrontFacingRatio(Sphere.Vertices, Sphere.Indices) > 0.99f);
	E_EXPECT_EQ(CountOpenEdges(Sphere.Vertices, Sphere.Indices), 0u); // 원본은 닫혀 있다 (검사 자체 확인)
}

E_TEST(Lod_SimplifyKeepsHardEdgesAndSkipsTinyMeshes)
{
	// 면마다 8x8 격자로 나눈 정육면체: 모서리(위치는 같고 법선이 다른 이음매)를 가로질러 붕괴하지 않으므로 면끼리 정점이 섞이지 않는다
	FMeshData Grid;
	constexpr uint32 N = 8;
	const FVector3   Axes[3] = { FVector3::ForwardVector, FVector3::RightVector, FVector3::UpVector };
	for (uint32 Face = 0; Face < 6; ++Face)
	{
		const FVector3 Normal = Axes[Face / 2] * ((Face % 2) == 0 ? 1.0f : -1.0f);
		const FVector3 U      = Axes[(Face / 2 + 1) % 3];
		const FVector3 V      = Axes[(Face / 2 + 2) % 3];
		const uint32   Base   = static_cast<uint32>(Grid.Vertices.size());
		for (uint32 Y = 0; Y <= N; ++Y)
		{
			for (uint32 X = 0; X <= N; ++X)
			{
				FVertex Vertex;
				Vertex.Position = Normal * 50.0f + U * (static_cast<float>(X) / N * 100.0f - 50.0f) + V * (static_cast<float>(Y) / N * 100.0f - 50.0f);
				Vertex.Normal   = Normal;
				Grid.Vertices.push_back(Vertex);
			}
		}
		for (uint32 Y = 0; Y < N; ++Y)
		{
			for (uint32 X = 0; X < N; ++X)
			{
				const uint32 I0 = Base + Y * (N + 1) + X;
				Grid.Indices.insert(Grid.Indices.end(), { I0, I0 + 1, I0 + N + 1, I0 + 1, I0 + N + 2, I0 + N + 1 });
			}
		}
	}
	const auto Indices = MeshSimplifier::SimplifyToTriangleCount(Grid.Vertices, Grid.Indices, 12);
	E_EXPECT_EQ(Indices.size(), static_cast<size_t>(36)); // 평면 면은 면마다 삼각형 2개까지 줄어든다
	FBox Bounds;
	for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
	{
		const FVector3 N0 = Grid.Vertices[Indices[Index]].Normal;
		E_EXPECT_EQUALS(Grid.Vertices[Indices[Index + 1]].Normal, N0, 1.0e-5f);
		E_EXPECT_EQUALS(Grid.Vertices[Indices[Index + 2]].Normal, N0, 1.0e-5f);
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Bounds.AddPoint(Grid.Vertices[Indices[Index + Corner]].Position);
		}
	}
	E_EXPECT_EQUALS(Bounds.Min, FVector3(-50.0f), 1.0e-4f); // 꼭짓점은 움직이지 않는다
	E_EXPECT_EQUALS(Bounds.Max, FVector3(50.0f), 1.0e-4f);
	E_EXPECT_NEAR(FrontFacingRatio(Grid.Vertices, Indices), FrontFacingRatio(Grid.Vertices, Grid.Indices), 1.0e-6f); // 와인딩 유지

	// 아주 작은 메시는 LOD를 만들지 않는다, LodCount 1이면 비운다
	FMeshData Small = FPrimitiveShapes::MakeCube(100.0f);
	MeshSimplifier::GenerateLods(Small, 4);
	E_EXPECT_TRUE(Small.Lods.empty());
	FMeshData Sphere = FPrimitiveShapes::MakeSphere(50.0f, 32, 16);
	MeshSimplifier::GenerateLods(Sphere, 1);
	E_EXPECT_TRUE(Sphere.Lods.empty());
}

E_TEST(Lod_ImportSettingsAndSerialization)
{
	FModelImportSettings Settings;
	E_EXPECT_TRUE(Settings.bGenerateLods);
	Settings.LodCount = 2;
	FModelImportSettings Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Settings.ToJsonString()));
	E_EXPECT_EQ(Loaded.LodCount, 2u);
	E_EXPECT_TRUE(Loaded.FromJsonString(R"({"LodCount": 99})"));
	E_EXPECT_EQ(Loaded.LodCount, LodMath::MaxLods); // 범위 고정

	// Apply: 정적 메시만 LOD 생성
	FModelData Model;
	FModelMesh& Static = Model.Meshes.emplace_back();
	Static.Data        = FPrimitiveShapes::MakeSphere(50.0f, 32, 16);
	FModelMesh& Skinned = Model.Meshes.emplace_back();
	Skinned.Data        = FPrimitiveShapes::MakeSphere(50.0f, 32, 16);
	Skinned.SkinVertices.resize(Skinned.Data.Vertices.size());
	FModelImportSettings{}.Apply(Model);
	E_EXPECT_EQ(Model.Meshes[0].Data.Lods.size(), static_cast<size_t>(3));
	E_EXPECT_TRUE(Model.Meshes[1].Data.Lods.empty());

	// 쿠킹 형식 왕복
	Model.Meshes.pop_back();
	FBinaryWriter Writer;
	FAssetCache::WriteModel(Writer, Model);
	FBinaryReader Reader(Writer.GetBuffer().data(), Writer.GetBuffer().size());
	FModelData    Read;
	E_EXPECT_TRUE(FAssetCache::ReadModel(Reader, Read));
	E_EXPECT_TRUE(Reader.IsAtEnd());
	E_EXPECT_EQ(Read.Meshes[0].Data.Lods.size(), static_cast<size_t>(3));
	E_EXPECT_TRUE(Read.Meshes[0].Data.Lods[2].Indices == Model.Meshes[0].Data.Lods[2].Indices);
	E_EXPECT_NEAR(Read.Meshes[0].Data.Lods[1].ScreenSize, Model.Meshes[0].Data.Lods[1].ScreenSize, 0.0f);

	// 범위 밖 LOD 인덱스는 거부
	Model.Meshes[0].Data.Lods[0].Indices[0] = static_cast<uint32>(Model.Meshes[0].Data.Vertices.size());
	FBinaryWriter BrokenWriter;
	FAssetCache::WriteModel(BrokenWriter, Model);
	FBinaryReader BrokenReader(BrokenWriter.GetBuffer().data(), BrokenWriter.GetBuffer().size());
	E_EXPECT_FALSE(FAssetCache::ReadModel(BrokenReader, Read));
}
